// Startup warm-up from this machine's record of the shaders and pipelines the submit thread had to create during
// earlier runs (DM_PSO_USER, written by shader_get / pso_get / the compute path; canonical shaders and explicitly
// recorded framebuffer-fetch transformations). It runs once, on the submit thread, at the game's first submission, so nothing
// else touches the shader and pipeline caches meanwhile:
//   1. recorded shaders load on worker threads (shader_load: file, source compile, reflection; c1/residency locked);
//   2. render pipeline descriptors are built serially through the draw path's own pso_desc (variant lookups included);
//   3. pipelines compile on worker threads, then go into the same caches pso_get and the compute path consult.
// Draws then find their objects in memory instead of creating libraries and pipelines inside a frame. Entries that no
// longer load (missing files, a changed key layout, variants not built) are skipped; ZELDA_SHADER_WARM=0 disables it.
#include "nvn_fetch_record.h"
#include "shader_status.h"
#include "nvn_pipeline_recipe.h"
static NvShader *zelda_fetch_shader(NvShader *, NSDictionary<NSString *, NSNumber *> *, NSString *);
static NvShader *zelda_cube_shader(NvShader *);
static NvShader *zelda_line_shader(NvShader *, NvShader *);
static NSDictionary<NSNumber *, NSArray *> *zelda_fetch_reads(NvShader *);
static NSSet<NSNumber *> *zelda_fetch_screen_outputs(NvShader *);
static void zelda_shader_warm(void) {
    static int done;
    if (done++) return;
    // Warm-up precedes the per-submit pool; its temporary descriptors and source strings can be released now.
    @autoreleasepool {
    const char *setting = getenv("ZELDA_SHADER_WARM");
    NSString *path = pso_user_path();
    if ((setting && !atoi(setting)) || !path) return;
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text.length) { MLOG("SHADER_WARM no record yet at %s", path.UTF8String); return; }
    uint64_t start = bp_now();
    shader_status_note(SHADER_CHECKING, 0);
    shmap_load();   // library directory (normally loaded at the first program lookup)
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    NSMutableArray<NSNumber *> *stages = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    NSMutableData *records = [NSMutableData data];
    NSMutableArray<NSString *> *fetches = [NSMutableArray array];
    NSMutableArray<NSNumber *> *vertices = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSArray<NSString *> *> *variants = [NSMutableDictionary dictionary];
    NSMutableArray<NSString *> *computes = [NSMutableArray array];
    void (^want)(NSString *, int) = ^(NSString *key, int stage) {
        if (!key.length || [seen containsObject:key] || g_shcache[key]) return;
        [seen addObject:key];
        NSString *file = [g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]];
        if (access(file.fileSystemRepresentation, R_OK)) return;
        [keys addObject:key]; [stages addObject:@(stage)];
    };
    unsigned malformed = 0, stale = 0, recipes = 0;
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
        if (c.count >= 3 && [c[0] isEqualToString:@"S"]) want(c[1], c[2].intValue);
        else if (c.count >= 2 && [c[0] isEqualToString:@"C"]) { want(c[1], 5); [computes addObject:c[1]]; }
        else if ([c[0] isEqualToString:@"V"]) {
            if (c.count != 3 || !zelda_fetch_record_key(c[1]) || !zelda_fetch_record_mapping(c[2])) { malformed++; continue; }
            want(c[1], 1);
            variants[[NSString stringWithFormat:@"%@|%@", c[1], c[2]]] = @[c[1], c[2]];
        }
        else if ([c[0] isEqualToString:@"P1"]) {
            pso_skey sk; unsigned vertex = 0; NSString *fetch = nil;
            int valid = zelda_pipeline_recipe_parse(c, g_msl_dir, &sk, &vertex, &fetch);
            if (valid <= 0) { if (valid < 0) malformed++; else stale++; continue; }
            want(warm_key(sk.vs, sizeof sk.vs), sk.vs_stage); want(warm_key(sk.fs, sizeof sk.fs), sk.fs_stage);
            [records appendBytes:&sk length:sizeof sk]; [fetches addObject:fetch]; [vertices addObject:@(vertex)]; recipes++;
        }
        else if (c.count >= 2 && ([c[0] isEqualToString:@"R"] || [c[0] isEqualToString:@"F"])) {
            pso_skey sk;
            if (pso_skey_parse(c[1].UTF8String, &sk)) { malformed++; continue; }
            if (sk.merged || sk.fz) continue;   // planned per pass; created at draw time
            NSString *fetch = [c[0] isEqualToString:@"F"] && c.count == 3 ? c[2] : @"";
            if ([c[0] isEqualToString:@"F"] && !zelda_fetch_record_mapping(fetch)) { malformed++; continue; }
            want(warm_key(sk.vs, sizeof sk.vs), sk.vs_stage); want(warm_key(sk.fs, sizeof sk.fs), sk.fs_stage);
            [records appendBytes:&sk length:sizeof sk];
            [fetches addObject:fetch];
            [vertices addObject:@0];
        }
    }
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    // 1. Shaders.
    NSUInteger count = keys.count;
    shader_status_note(SHADER_LIBRARIES, (unsigned)count);
    __strong NvShader **loaded = (__strong NvShader **)calloc(count ? count : 1, sizeof(NvShader *));
    dispatch_apply(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { @autoreleasepool {
        loaded[i] = shader_load(keys[i], stages[i].intValue);
        shader_status_note(SHADER_STEP, 0);
    } });
    unsigned shaders = 0;
    for (NSUInteger i = 0; i < count; i++) {
        if (loaded[i] && !g_shcache[keys[i]]) { g_shcache[keys[i]] = loaded[i]; atomic_fetch_add(&g_shcache_n, 1); shaders++; }
        loaded[i] = nil;
    }
    free(loaded);
    uint64_t shaders_done = bp_now();
    shader_status_note(SHADER_PIPELINES, 0);
    // 2. Render pipeline descriptors, serially through the draw path's construction.
    size_t total = records.length / sizeof(pso_skey);
    const pso_skey *recorded = records.bytes;
    NSMutableData *pipeline_data = [NSMutableData data];
    NSMutableArray *descriptors = [NSMutableArray array];
    NSMutableSet<NSData *> *pending_keys = [NSMutableSet set];
    unsigned fetch_pipelines = 0;
    z_warming = 1;
    unsigned fetch_libraries = 0;
    for (NSArray<NSString *> *recipe in variants.allValues) {
        NvShader *shader = g_shcache[recipe[0]]; if (!shader) continue;
        zelda_fetch_reads(shader);
        if (zelda_fetch_shader(shader, zelda_fetch_record_mapping(recipe[1]), recipe[1])) fetch_libraries++;
    }
    const char *attachment_setting = getenv("ZELDA_WARM_ATTACHMENT_VARIANTS");
    int warm_attachments = attachment_setting ? atoi(attachment_setting) : 2; // 1: previous fetch-only coverage, 0: none
    NSUInteger direct_count = 0;
    // Queue observed keys first, then subsets of unwritten attachments not read by framebuffer fetch.
    for (unsigned derived = 0; derived <= (unsigned)!!warm_attachments; derived++) {
    for (size_t i = 0; i < total; i++) {
        if (derived && warm_attachments < 2 && !fetches[i].length) continue;
        const pso_skey *sk = &recorded[i];
        NSString *vkey = warm_key(sk->vs, sizeof sk->vs), *fkey = warm_key(sk->fs, sizeof sk->fs);
        NvShader *vs = vkey.length ? g_shcache[vkey] : nil, *fs = fkey.length ? g_shcache[fkey] : nil;
        if (!vs || (fkey.length && !fs)) continue;
        if (vertices[i].unsignedIntValue == 1) vs = zelda_cube_shader(vs);
        else if (vertices[i].unsignedIntValue == 2) vs = zelda_line_shader(vs, fs);
        if (!vs) continue;
        NSDictionary<NSString *, NSNumber *> *mapping = fetches[i].length ? zelda_fetch_record_mapping(fetches[i]) : nil;
        if (fetches[i].length) {
            if (!fs) continue;
            // Runtime eligibility still checks every binding; prime its unchanged source proofs before gameplay.
            zelda_fetch_reads(fs); zelda_fetch_screen_outputs(vs);
            fs = zelda_fetch_shader(fs, mapping, fetches[i]);
            if (!fs) continue;
        }
        pso_key base = sk->k; base.vs = (__bridge void *)vs; base.fs = (__bridge void *)fs; base.fsfn = NULL;
        unsigned fetched = 0;
        for (NSNumber *slot in mapping.allValues) fetched |= 1u << slot.unsignedIntValue;
        unsigned optional = derived ? zelda_pso_optional_colors(&base, fetched) : 0;
        if (derived && !optional) continue;
        for (unsigned removed = derived ? 1 : 0; removed <= (derived ? 255u : 0u); removed++) {
        if (removed & ~optional) continue;
        pso_key k = base;
        for (unsigned slot = 0; slot < 8; slot++) if (removed >> slot & 1) k.cfmt[slot] = 0;
        zelda_pso_normalize(&k, 0);
        NSData *identity = [NSData dataWithBytes:&k length:sizeof k];
        if (hm_get_id(&g_pso, &k) || [pending_keys containsObject:identity]) continue;
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
        if (!d.vertexFunction || (fs && !d.fragmentFunction)) continue;   // variant library not built
        [pending_keys addObject:identity];
        [pipeline_data appendBytes:&k length:sizeof k]; [descriptors addObject:d];
        if (!derived && fetches[i].length) fetch_pipelines++;
        }
    }
    if (!derived) direct_count = descriptors.count;
    }
    z_warming = 0;
    NSMutableArray<NvShader *> *compute_shaders = [NSMutableArray array];
    for (NSString *key in computes) {
        NvShader *shader = g_shcache[key];
        const void *ck = (__bridge const void *)shader;
        if (shader && shader->fn && !hm_get_id(&g_cps, &ck) && ![compute_shaders containsObject:shader]) [compute_shaders addObject:shader];
    }
    // 3. Pipelines.
    NSUInteger rendered = descriptors.count, computed = compute_shaders.count;
    shader_status_note(SHADER_PIPELINES, (unsigned)(rendered + computed));
    const pso_key *pipeline_keys = pipeline_data.bytes;
    __strong id *built = (__strong id *)calloc(rendered + computed + 1, sizeof(id));
    dispatch_apply(rendered + computed, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { @autoreleasepool {
        if (i < rendered) built[i] = ZELDA_SHADER_BUILD([g_dev newRenderPipelineStateWithDescriptor:descriptors[i] error:NULL]);
        else built[i] = ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:compute_shaders[i - rendered]->fn error:NULL]);
        shader_status_note(SHADER_STEP, 0);
    } });
    unsigned pipelines = 0, compute = 0, failed = 0, derived_failed = 0;
    for (NSUInteger i = 0; i < rendered + computed; i++) {
        if (!built[i]) { if (i >= direct_count && i < rendered) derived_failed++; else failed++; }
        else if (i < rendered) { if (!hm_get_id(&g_pso, &pipeline_keys[i])) { hm_put_id(&g_pso, &pipeline_keys[i], built[i]); pipelines++; } }
        else { const void *ck = (__bridge const void *)compute_shaders[i - rendered]; hm_put_id(&g_cps, &ck, built[i]); compute++; }
        built[i] = nil;
    }
    free(built);
    shader_status_note(SHADER_FINISHED, 0);
    MLOG("SHADER_WARM shaders=%u/%lu pipelines=%u/%lu records=%zu fetch_descriptors=%u derived_descriptors=%lu derived_failed=%u fetch_libraries=%u compute=%u failed=%u malformed=%u recipes=%u stale=%u shader_ms=%.1f total_ms=%.1f",
         shaders, (unsigned long)count, pipelines, (unsigned long)rendered, total, fetch_pipelines,
         (unsigned long)(rendered - direct_count), derived_failed, fetch_libraries, compute, failed, malformed, recipes, stale,
         (shaders_done - start) / 1e6, (bp_now() - start) / 1e6);
    shader_ir_report("warm");
    }
}
