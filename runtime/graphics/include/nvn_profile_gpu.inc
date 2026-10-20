// Metal 4 encoder timestamps; enabled only for explicit profiling runs.
enum { Z_PROFILE_SLOTS = 4096, Z_PROFILE_RING = 8 };
typedef struct {
    uint64_t frame, pass; unsigned slot, idx, width, height, color, depth, draws; char kind;
    char first_vs[17], first_fs[17];
} ZeldaGPUProfile;
static FILE *z_profile_file;
static int z_profile_on = -1, z_profile_bank, z_profile_open = -1;
static id<MTL4CounterHeap> z_profile_heaps[Z_PROFILE_RING];
static __unsafe_unretained id z_profile_encoder;
static ZeldaGPUProfile z_profile_entries[Z_PROFILE_SLOTS / 2];
static unsigned z_profile_count, z_profile_used, z_profile_index;
static uint64_t z_profile_frame = UINT64_MAX;
static uint64_t z_profile_frequency;
static pthread_mutex_t z_profile_lock = PTHREAD_MUTEX_INITIALIZER;

static int zelda_profile_enabled(void) {
    if (z_profile_on >= 0) return z_profile_on;
    z_profile_on = 0;
    const char *path = getenv("DM_NVN_PASSGPU");
    if (!path || !*path) return 0;
    z_profile_frequency = [g_dev queryTimestampFrequency];
    if (!z_profile_frequency) MDIE("GPU timestamp frequency unavailable");
    MTL4CounterHeapDescriptor *d = [MTL4CounterHeapDescriptor new];
    d.type = MTL4CounterHeapTypeTimestamp; d.count = Z_PROFILE_SLOTS;
    for (unsigned i = 0; i < Z_PROFILE_RING; i++) {
        NSError *error = nil; z_profile_heaps[i] = [g_dev newCounterHeapWithDescriptor:d error:&error];
        if (!z_profile_heaps[i]) MDIE("Metal 4 timestamp heap unavailable: %s", error.localizedDescription.UTF8String);
    }
    z_profile_file = fopen(path, "w");
    if (!z_profile_file) MDIE("Cannot open GPU profile output");
    fputs("frame\tidx\tkind\tgpu_ms\tw\th\tfmt0\tdfmt\tdraws\tt0_ns\tt1_ns\tpass\tfirst_vs\tfirst_fs\n", z_profile_file);
    return z_profile_on = 1;
}
static void pg_sub_begin(uint64_t fence) {
    if (!zelda_profile_enabled()) return;
    if (fence > Z_PROFILE_RING && !mtl_wait(fence - Z_PROFILE_RING, UINT64_MAX)) MDIE("GPU profile heap still in use");
    z_profile_bank = fence % Z_PROFILE_RING;
    [z_profile_heaps[z_profile_bank] invalidateCounterRange:NSMakeRange(0, Z_PROFILE_SLOTS)];
    z_profile_count = z_profile_used = 0; z_profile_open = -1; z_profile_encoder = nil;
}
static void zelda_profile_stamp(id encoder, const ZeldaGPUProfile *p, int end) {
    id<MTL4CounterHeap> heap = z_profile_heaps[z_profile_bank];
    if (p->kind == 'R') {
        id<MTL4RenderCommandEncoder> render = encoder;
        [render writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:heap atIndex:p->slot + 2 * end];
        [render writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageFragment intoHeap:heap atIndex:p->slot + 2 * end + 1];
    } else [(id<MTL4ComputeCommandEncoder>)encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:heap atIndex:p->slot + end];
}
static void zelda_profile_begin(id encoder, MTL4RenderPassDescriptor *descriptor) {
    if (!zelda_profile_enabled()) return;
    if (z_profile_open >= 0) MDIE("Overlapping GPU profile encoders");
    unsigned slots = descriptor ? 4 : 2;
    if (z_profile_used + slots > Z_PROFILE_SLOTS) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (frame != z_profile_frame) { z_profile_frame = frame; z_profile_index = 0; }
    ZeldaGPUProfile *p = &z_profile_entries[z_profile_count];
    *p = (ZeldaGPUProfile){.frame=frame,.pass=g_stat_pass,.slot=z_profile_used,.idx=z_profile_index++,
        .kind=descriptor ? 'R' : 'C',.draws=(unsigned)g_stat_draw};
    if (descriptor) {
        id<MTLTexture> color = descriptor.colorAttachments[0].texture, depth = descriptor.depthAttachment.texture;
        id<MTLTexture> size = color ?: depth;
        NSUInteger level = color ? descriptor.colorAttachments[0].level : descriptor.depthAttachment.level;
        p->color = color.pixelFormat; p->depth = depth.pixelFormat;
        p->width = size ? MAX(size.width >> level, 1) : descriptor.renderTargetWidth;
        p->height = size ? MAX(size.height >> level, 1) : descriptor.renderTargetHeight;
    }
    z_profile_open = z_profile_count++; z_profile_used += slots; z_profile_encoder = encoder;
    zelda_profile_stamp(encoder, p, 0);
}
static void zelda_profile_end(id encoder) {
    if (z_profile_open < 0 || encoder != z_profile_encoder) return;
    ZeldaGPUProfile *p = &z_profile_entries[z_profile_open];
    zelda_profile_stamp(encoder, p, 1); p->draws = (unsigned)g_stat_draw - p->draws; p->pass = g_stat_pass;
    z_profile_open = -1; z_profile_encoder = nil;
}
static void zelda_profile_draw(const char *vs, const char *fs) {
    if (z_profile_open < 0) return;
    ZeldaGPUProfile *p = &z_profile_entries[z_profile_open];
    if (p->kind != 'R' || p->first_vs[0]) return;
    snprintf(p->first_vs, sizeof p->first_vs, "%.16s", vs ?: "-");
    snprintf(p->first_fs, sizeof p->first_fs, "%.16s", fs ?: "-");
}
static dispatch_block_t zelda_profile_commit(void) {
    if (!z_profile_count) return nil;
    NSData *entries = [NSData dataWithBytes:z_profile_entries length:z_profile_count * sizeof(ZeldaGPUProfile)];
    id<MTL4CounterHeap> heap = z_profile_heaps[z_profile_bank]; unsigned used = z_profile_used;
    return ^{
        @autoreleasepool {
            NSData *data = [heap resolveCounterRange:NSMakeRange(0, used)];
            if (data.length < used * sizeof(MTL4TimestampHeapEntry)) MDIE("Incomplete GPU timestamp results");
            const MTL4TimestampHeapEntry *times = data.bytes; const ZeldaGPUProfile *ps = entries.bytes;
            pthread_mutex_lock(&z_profile_lock);
            for (unsigned i = 0; i < entries.length / sizeof *ps; i++) {
                const ZeldaGPUProfile *p = &ps[i];
                uint64_t a = times[p->slot].timestamp, b = times[p->slot + 1].timestamp;
                if (p->kind == 'R') { a = MIN(a, b); b = MAX(times[p->slot + 2].timestamp, times[p->slot + 3].timestamp); }
                if (!a || b < a) continue;
                fprintf(z_profile_file, "%llu\t%u\t%c\t%.6f\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%s\t%s\n",
                    (unsigned long long)p->frame, p->idx, p->kind, (b - a) * 1000.0 / z_profile_frequency, p->width, p->height, p->color, p->depth, p->draws,
                    (unsigned long long)(a * (1e9L / z_profile_frequency)), (unsigned long long)(b * (1e9L / z_profile_frequency)), (unsigned long long)p->pass, p->first_vs, p->first_fs);
            }
            fflush(z_profile_file); pthread_mutex_unlock(&z_profile_lock);
        }
    };
}
static inline void args_dirty_all(void);
static id<MTL4ComputeCommandEncoder> pg_compute(void) { ser_gate(); args_dirty_all(); return enc_c_begin(NULL); }
#define pg_render(descriptor, count) ((void)0)
#define pg_end_render() ((void)0)
