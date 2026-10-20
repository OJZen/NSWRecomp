// NVN objects can describe the same device storage; swizzles belong to their sampled views.
// shortcut: partial or incompatible-format aliases retain the old path; add reinterpretation when exercised.
static NSMapTable<NSData *, NvPkg *> *z_texture_storage;
static uint64_t z_texture_alias_hits;
static id<MTLBuffer> zelda_texture_pool(uint64_t address, uint64_t length, uint64_t *offset) {
    if (length > UINT64_MAX - address) return nil;
    static int legacy = -1;
    if (legacy < 0) { const char *e = getenv("ZELDA_ALIAS_RESIDENCY"); legacy = e && atoi(e); }
    if (legacy) { // A/B: the previous identity lookup also made the whole pool resident.
        void *buffer = NULL;
        return mtl_addr(address, length, &buffer, offset) ? (__bridge id<MTLBuffer>)buffer : nil;
    }
    // Holding an identity reference does not mean the GPU reads the pool's bytes.
    pthread_mutex_lock(&g_mu);
    int i = zelda_pool_find(address, length);
    id<MTLBuffer> buffer = i >= 0 ? g_pools[i].buf : nil;
    if (i >= 0) *offset = address - g_pools[i].base;
    pthread_mutex_unlock(&g_mu);
    return buffer;
}
// Render targets of the game's virtual, CPU-inaccessible pools share memory as they do on the console: the game
// places its transient targets at overlapping offsets of one such pool (Great Sky Island: 354 targets, 373 MiB of
// layouts in a 111 MiB pool), and giving each its own Metal texture tripled that. Each such pool gets a placement
// heap S times its size; a target goes at its guest offset x S only if its Metal size and alignment fit inside its
// guest range x S, so targets that do not overlap in guest memory cannot overlap in the heap, and those that do
// overlap share memory exactly where the game reuses it. Everything else keeps its own texture. The game never
// writes these pools from the CPU, so no guest data is uploaded into them; every encoder starts behind a queue
// barrier, which orders a target's last use before the next one's writes. ZELDA_RT_HEAP=0 disables it.
static double z_rt_heap_scale = 1.25;   // Great Sky Island: 1.125 left 56 MiB of targets outside; 1.25 places all but 3 MiB
static struct { const pool_t *pool; uint64_t size; __strong id<MTLHeap> heap; } z_rt_heaps[8];
static uint64_t z_rt_heap_placed, z_rt_heap_bytes, z_rt_heap_fallbacks, z_rt_heap_fallback_bytes;
static id<MTLTexture> zelda_heap_texture(const tex_t *t, MTLTextureDescriptor *d, uint64_t offset) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("ZELDA_RT_HEAP"), *scale = getenv("ZELDA_RT_HEAP_SCALE");   // scale: experiments only
        enabled = !e || atoi(e);
        if (scale && atof(scale) >= 1) z_rt_heap_scale = atof(scale);
    }
    const uint32_t cpu_no_access = 0x1, virtual_pool = 0x200;   // NVN_MEMORY_POOL_FLAGS_*
    if (!enabled || !t->pool || (t->pool->flags & (cpu_no_access | virtual_pool)) != (cpu_no_access | virtual_pool) ||
        (t->d.flags & NVN_TEXTURE_FLAGS_DISPLAY) || !(d.usage & MTLTextureUsageRenderTarget) ||
        d.storageMode != MTLStorageModePrivate || !t->L.size || offset + t->L.size > t->pool->size) return nil;
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (z_rt_heaps[i].pool == t->pool && z_rt_heaps[i].size == t->pool->size) slot = i;
    if (slot < 0) {
        for (int i = 0; i < 8 && slot < 0; i++) if (!z_rt_heaps[i].heap) slot = i;
        if (slot < 0) return nil;
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate;
        hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        hd.size = ((uint64_t)ceil(t->pool->size * z_rt_heap_scale) + 0xffff) & ~(uint64_t)0xffff;
        id<MTLHeap> heap = [g_dev newHeapWithDescriptor:hd];
        if (!heap) { MLOG("ERROR: Render-target heap allocation failed: %.1f MiB", hd.size / 1048576.0); return nil; }
        res_add((id<MTLResource>)heap);   // residency covers every texture placed in it
        z_rt_heaps[slot].pool = t->pool; z_rt_heaps[slot].size = t->pool->size; z_rt_heaps[slot].heap = heap;   // a replaced heap lives on in its textures
        MLOG("Render-target heap: guest virtual pool %.1f MiB -> Metal heap %.1f MiB", t->pool->size / 1048576.0, hd.size / 1048576.0);
    }
    id<MTLHeap> heap = z_rt_heaps[slot].heap;
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    uint64_t place = ((uint64_t)ceil(offset * z_rt_heap_scale) + need.align - 1) / need.align * need.align;
    uint64_t limit = (uint64_t)floor((offset + t->L.size) * z_rt_heap_scale);
    if (place + need.size > limit || place + need.size > heap.size) { z_rt_heap_fallbacks++; z_rt_heap_fallback_bytes += need.size; return nil; }
    id<MTLTexture> texture = [heap newTextureWithDescriptor:d offset:place];
    if (!texture) { z_rt_heap_fallbacks++; return nil; }
    z_rt_heap_placed++; z_rt_heap_bytes += need.size;
    return texture;
}

static id<MTLTexture> zelda_device_texture(const tex_t *t, MTLTextureDescriptor *d) {
    uint64_t offset = 0;
    id<MTLBuffer> memory = t->mem ? zelda_texture_pool((uint64_t)t->mem, t->L.size, &offset) : nil;
    NSData *key = nil;
    if (memory) {
        // The holder retains this pool buffer, so a reused host address cannot match a retired pool.
        uint64_t fields[] = {(uintptr_t)(__bridge void *)memory, offset, t->L.size, t->L.layer_size,
            d.textureType, d.pixelFormat, d.width, d.height, d.depth, d.arrayLength,
            d.mipmapLevelCount, d.sampleCount, d.usage, t->d.flags, t->d.stride,
            (uint64_t)t->d.pkg_block_h, t->L.tileH, t->L.tileD};
        key = [NSData dataWithBytes:fields length:sizeof fields];
        if (!z_texture_storage) z_texture_storage = [NSMapTable weakToWeakObjectsMapTable];
        NvPkg *shared = [z_texture_storage objectForKey:key];
        if (shared) { serial_put(t->serial, shared); z_texture_alias_hits++; return shared->tex; }
    }
    NvPkg *holder = [NvPkg new];
    holder->tex = memory ? zelda_heap_texture(t, d, offset) : nil;
    if (!holder->tex) {
        holder->tex = [g_dev newTextureWithDescriptor:d];
        if (!holder->tex) return nil;
        res_add(holder->tex);
    }
    holder->storage_key = key; holder->storage_pool = memory;
    if (key) [z_texture_storage setObject:holder forKey:key];
    serial_put(t->serial, holder);
    return holder->tex;
}

// DM_RES_CENSUS: live render-target storage against the guest pool ranges it stands for. The console aliases
// transient targets in its pools; here each storage key has its own Metal texture.
static void zelda_texture_alias_census(void) {
    struct range { uint64_t pool, begin, end; };
    struct range *ranges = calloc(4096, sizeof *ranges); unsigned n = 0;   // guest threads have small stacks
    if (!ranges) return;
    uint64_t metal = 0, guest = 0, covered = 0;
    for (NSData *key in z_texture_storage.keyEnumerator.allObjects) {
        const uint64_t *f = key.bytes; NvPkg *holder = [z_texture_storage objectForKey:key];
        if (!holder || !(f[12] & MTLTextureUsageRenderTarget) || n == 4096) continue;
        metal += holder->tex.allocatedSize; guest += f[2];
        ranges[n++] = (struct range){f[0], f[1], f[1] + f[2]};
    }
    qsort_b(ranges, n, sizeof *ranges, ^int(const void *a, const void *b) {
        const struct range *x = a, *y = b;
        return x->pool != y->pool ? (x->pool < y->pool ? -1 : 1) : x->begin < y->begin ? -1 : x->begin > y->begin;
    });
    NSMutableString *pools = [NSMutableString string];
    for (unsigned i = 0; i < n;) {   // union per pool, plus each pool's size, extent and target count
        uint64_t pool = ranges[i].pool, first = ranges[i].begin, last = 0, pool_covered = 0; unsigned targets = 0;
        while (i < n && ranges[i].pool == pool) {
            uint64_t begin = ranges[i].begin, end = ranges[i].end;
            for (i++, targets++; i < n && ranges[i].pool == pool && ranges[i].begin <= end; i++, targets++) if (ranges[i].end > end) end = ranges[i].end;
            pool_covered += end - begin; if (end > last) last = end;
        }
        covered += pool_covered;
        if (pool_covered >= (4u << 20))
            [pools appendFormat:@" pool=%.0fM targets=%u range=%.1f-%.1fM union=%.1fM", ((__bridge id<MTLBuffer>)(void *)pool).length / 1048576.0, targets,
                first / 1048576.0, last / 1048576.0, pool_covered / 1048576.0];
    }
    free(ranges);
    (void)metal; (void)guest; (void)covered;   // log-only values
    MLOG("Render-target storage: targets=%u Metal=%.1f MiB guest_descriptors=%.1f MiB guest_union=%.1f MiB;%s; heap_placed=%llu %.1f MiB fallback=%llu %.1f MiB",
         n, metal / 1048576.0, guest / 1048576.0, covered / 1048576.0, pools.UTF8String,
         z_rt_heap_placed, z_rt_heap_bytes / 1048576.0, z_rt_heap_fallbacks, z_rt_heap_fallback_bytes / 1048576.0);
}
static id<MTLTexture> tex_mtl(const tex_t *t);
static int zelda_texture_same_storage(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    if (a->serial && a->serial == b->serial) return 1;
    if (!a->mem || a->mem != b->mem || a->L.size != b->L.size) return 0;
    // Different layouts can overlap guest memory but still have separate Metal storage.
    id<MTLTexture> source = tex_mtl(a);
    return source && source == tex_mtl(b);
}

// This version's final upscale shader reads scene slot 0; report size changes without a heavy pass dump.
#include "performance.h"
static void zelda_note_render_input(const char *shader, int reg, id<MTLTexture> texture) {
    static NSUInteger width, height;
    if (reg || !texture || strcmp(shader, "cf0fd3303967d34b_b")) return;
    if (width == texture.width && height == texture.height) return;
    width = texture.width; height = texture.height;
    zelda_perf_render_size((unsigned)width, (unsigned)height);  // shown in the performance panel
    MLOG("RENDER_INPUT frame=%llu width=%lu height=%lu shader=%s", (unsigned long long)atomic_load(&g_cur_frame),
        (unsigned long)width, (unsigned long)height, shader);
}
