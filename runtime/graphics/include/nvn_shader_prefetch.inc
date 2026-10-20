#include "shader_status.h"
// Programs are usually created while the game loads, frames before their first draw. Compiling their libraries at
// nvnProgramSetShaders (same source transform and options as lib_from_file) lets the draw-time load hit the system
// compiler cache instead of compiling MSL inside a frame. Only that cache is warmed: shader objects are still built
// on the submit thread, which waits for a compile already running but never for one that has not started.
enum { Z_PREFETCH_QUEUED = 1, Z_PREFETCH_RUNNING, Z_PREFETCH_DONE };
static NSMutableDictionary<NSString *, NSNumber *> *z_prefetch_state;
static pthread_mutex_t z_prefetch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t z_prefetch_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t z_prefetch_compiled, z_prefetch_waits, z_prefetch_wait_ns, z_prefetch_started_late;

static NSString *zelda_prefetch_dir(void) {
    static NSString *directory; static dispatch_once_t once;
    // Same directory shmap_load derives from DM_MSL_DIR; without it nothing is prefetched.
    dispatch_once(&once, ^{ const char *m = getenv("DM_MSL_DIR"); if (m && *m) directory = [NSString stringWithFormat:@"%s/b-%s", m, DMSH_CHAIN]; });
    return directory;
}

static void zelda_prefetch_run(NSString *name) {
    pthread_mutex_lock(&z_prefetch_mu);
    int queued = z_prefetch_state[name].intValue == Z_PREFETCH_QUEUED;
    if (queued) z_prefetch_state[name] = @(Z_PREFETCH_RUNNING);
    pthread_mutex_unlock(&z_prefetch_mu);
    if (!queued) return;
    @autoreleasepool {
        NSString *path = [zelda_prefetch_dir() stringByAppendingPathComponent:[name stringByAppendingString:@".metallib"]];
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data.length >= 4 && memcmp(data.bytes, "MTLB", 4) && !shader_ir_load(g_dev, path, data.bytes, data.length, false)) {
            NSString *source = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            MTLCompileOptions *options = nil;
            if (source) source = zelda_shader_math(source, &options);
            // Failures are reported by the draw-time load of the same file.
            if (source && ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:source options:options error:NULL])) atomic_fetch_add(&z_prefetch_compiled, 1);
        }
    }
    pthread_mutex_lock(&z_prefetch_mu);
    z_prefetch_state[name] = @(Z_PREFETCH_DONE);
    pthread_cond_broadcast(&z_prefetch_cv);
    pthread_mutex_unlock(&z_prefetch_mu);
}

static void zelda_prefetch(NSString *name) {
    NSString *directory = zelda_prefetch_dir();
    if (!directory || !g_dev) return;
    static dispatch_queue_t feeder, workers; static dispatch_semaphore_t slots; static dispatch_once_t once;
    dispatch_once(&once, ^{
        pthread_mutex_lock(&z_prefetch_mu); z_prefetch_state = [NSMutableDictionary dictionary]; pthread_mutex_unlock(&z_prefetch_mu);
        feeder = dispatch_queue_create("zelda.shader-prefetch", DISPATCH_QUEUE_SERIAL);
        workers = dispatch_queue_create("zelda.shader-prefetch.compile",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
        slots = dispatch_semaphore_create(1);
    });
    pthread_mutex_lock(&z_prefetch_mu);
    int known = z_prefetch_state[name] != nil;
    pthread_mutex_unlock(&z_prefetch_mu);
    if (known || access([directory stringByAppendingPathComponent:[name stringByAppendingString:@".metallib"]].fileSystemRepresentation, R_OK)) return;
    pthread_mutex_lock(&z_prefetch_mu);
    known = z_prefetch_state[name] != nil;
    if (!known) z_prefetch_state[name] = @(Z_PREFETCH_QUEUED);
    pthread_mutex_unlock(&z_prefetch_mu);
    if (known) return;
    // Keep shader prefetch to one compiler on the shared development machine.
    dispatch_async(feeder, ^{
        dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
        dispatch_async(workers, ^{ zelda_prefetch_run(name); dispatch_semaphore_signal(slots); });
    });
}

// Called from nvnProgramSetShaders: each stage's library and the float-attribute vertex/fragment pair the draw
// path selects first (zelda_vertex_function with mask 0).
void mtl_prog_prefetch(const NVNshaderData *shaders, int count) {
    char keys[6][17] = {{0}}; int stages[6];
    for (int i = 0; i < count && i < 6; i++) {
        uint8_t hash[32]; stages[i] = -1;
        if (!zelda_shader_hash(shaders[i].control, (const void *)(uintptr_t)shaders[i].data, hash)) continue;
        for (unsigned j = 0; j < 8; j++) snprintf(keys[i] + 2 * j, 3, "%02x", hash[j]);
        stages[i] = (int)shader_word(shaders[i].control, 0x714);
        zelda_prefetch([@(keys[i]) stringByAppendingString:@"_b"]);
    }
    if (count != 2) return;
    int vertex = stages[0] == 0 ? 0 : stages[1] == 0 ? 1 : -1, fragment = 1 - vertex;
    if (vertex < 0 || stages[fragment] != 1) return;
    char identity[64], name[20]; uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    snprintf(identity, sizeof identity, "%s-00000000-%s", keys[vertex], keys[fragment]);
    CC_SHA256(identity, (CC_LONG)strlen(identity), hash);
    for (unsigned j = 0; j < 8; j++) snprintf(name + 2 * j, 3, "%02x", hash[j]);
    zelda_prefetch([@(name) stringByAppendingString:@"_b"]);
}

// R records use the cached shader objects themselves: variants reuse base keys and need their own reconstruction
// recipe. Framebuffer fetch has F records; layered and wide-line pipelines carry versioned P1 recipes.
static inline int zelda_warm_canonical(NvShader *vs, NvShader *fs) {
    return vs && g_shcache[@(vs->key)] == vs && (!fs || g_shcache[@(fs->key)] == fs);
}

// Submit thread, before a library is loaded: join a running prefetch, drop a queued one, and remember the name.
static void zelda_prefetch_wait(NSString *name) {
    pthread_mutex_lock(&z_prefetch_mu);
    if (!z_prefetch_state) { pthread_mutex_unlock(&z_prefetch_mu); return; }
    int state = z_prefetch_state[name].intValue;
    if (state == Z_PREFETCH_QUEUED) atomic_fetch_add(&z_prefetch_started_late, 1);
    if (state == Z_PREFETCH_RUNNING) {
        uint64_t start = bp_now(); atomic_fetch_add(&z_prefetch_waits, 1);
        while (z_prefetch_state[name].intValue == Z_PREFETCH_RUNNING) pthread_cond_wait(&z_prefetch_cv, &z_prefetch_mu);
        atomic_fetch_add(&z_prefetch_wait_ns, bp_now() - start);
    }
    z_prefetch_state[name] = @(Z_PREFETCH_DONE);
    pthread_mutex_unlock(&z_prefetch_mu);
}
