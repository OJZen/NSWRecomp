// Packaged textures live in guest-visible pools: they are uploaded from guest memory and re-uploaded when their pages
// are written. Some are GPU destinations too (render targets, image stores, copies). v1.4.3 renders each terrain tile's
// RG8 normal map (260x260, 9 levels, MINIMAL_LAYOUT) into a CPU-cached pool while loading, and later moves tiles inside
// that pool with CPU copies: on the console the copy carries the rendered texels, here they existed only in the Metal
// texture, so moved tiles read zeros (grass blades lay flat, terrain lost its relief shading). Three rules follow:
//  - Packaged objects with identical storage, layout and descriptor share one holder, as device textures do
//    (nvn_texture_alias.inc); both would upload the same guest bytes anyway.
//  - A GPU-written texture keeps its contents unless the guest bytes of its own range really changed. Per-page hashes
//    are taken when the GPU first writes it, and the pages marked dirty (16 KiB host pages are shared with neighbouring
//    data) are compared before a re-upload.
//  - The first GPU write after a quiet spell is written back into guest memory in the guest layout at the end of the
//    same submission (nvn_texture_writeback.inc), so guest copies see the texels. Targets rendered every frame are
//    written back only at the start of such a spell; their later contents stay GPU-only, as before.
static NSMapTable<NSData *, NvPkg *> *z_packaged_storage;
static uint64_t z_packaged_shared;
static NSData *zelda_packaged_key(const tex_t *t) {
    const nvn_texdesc *d = &t->d; const nvn_texlayout *L = &t->L;
    uint64_t fields[] = {(uintptr_t)t->mem, (uintptr_t)t->pool, t->has_layout, 0, t->swizzle,
        (uint64_t)d->target, (uint64_t)d->format, (uint64_t)d->flags, (uint64_t)d->samples, d->width, d->height, d->depth, d->levels, d->stride,
        (uint64_t)d->pkg_block_h, L->size, L->layer_size, L->align, L->layers, L->bpb, L->tileH, L->tileD};
    memcpy(&fields[3], t->layout, 8);
    return [NSData dataWithBytes:fields length:sizeof fields];
}
static NvPkg *zelda_packaged_shared_holder(const tex_t *t) {
    if (!z_packaged_storage || !t->mem) return nil;
    NvPkg *shared = [z_packaged_storage objectForKey:zelda_packaged_key(t)];
    if (shared && !z_packaged_shared++) MLOG("Upload texture sharing: first ser=%llu %ux%u format=%#x reuses matching storage and descriptor", t->serial, t->d.width, t->d.height, t->d.format);
    return shared;
}
static void zelda_packaged_register(const tex_t *t, NvPkg *pk) {
    if (!t->mem) return;
    if (!z_packaged_storage) z_packaged_storage = [NSMapTable weakToWeakObjectsMapTable];
    pk->storage_key = zelda_packaged_key(t);   // the holder keeps its key alive, so the entry goes with the holder
    [z_packaged_storage setObject:pk forKey:pk->storage_key];
}

enum { Z_WRITEBACK_QUIET_FRAMES = 30 };
static uint64_t z_rendered_marked, z_rendered_kept, z_rendered_changed;
static NSMutableArray *z_writeback_pending;   // @[holder, NSData(tex_t)] for the current submission
static uint64_t zelda_rendered_hash(const uint8_t *p, size_t n) {
    uint64_t h[4] = {0x9e3779b97f4a7c15ull ^ n, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) { uint64_t w; memcpy(&w, p + i + 8 * k, 8); h[k] = (h[k] ^ w) * 0xff51afd7ed558ccdull; h[k] ^= h[k] >> 29; }
    for (; i < n; i++) h[0] = (h[0] ^ p[i]) * 0x100000001b3ull;
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
}
// Guest range of host page `i` of the texture (pages counted from the one holding its first byte).
static size_t zelda_rendered_page(const tex_t *t, size_t i, uintptr_t *start) {
    uintptr_t page = (uintptr_t)getpagesize(), lo = (uintptr_t)t->mem, hi = lo + t->L.size, base = (lo / page + i) * page;
    *start = MAX(lo, base);
    return MIN(hi, base + page) - *start;
}
static NSData *zelda_rendered_hashes(const tex_t *t) {
    uintptr_t page = (uintptr_t)getpagesize(), lo = (uintptr_t)t->mem;
    size_t n = (lo + t->L.size + page - 1) / page - lo / page;
    NSMutableData *hashes = [NSMutableData dataWithLength:n * sizeof(uint64_t)];
    uint64_t *h = hashes.mutableBytes;
    for (size_t i = 0; i < n; i++) { uintptr_t a; size_t len = zelda_rendered_page(t, i, &a); h[i] = zelda_rendered_hash((const uint8_t *)a, len); }
    return hashes;
}
static void zelda_texture_gpu_written(const tex_t *t) {
    if (!t || !t->serial || !t->mem || !t->L.size) return;
    id holder = serial_get(t->serial);
    if (![holder isKindOfClass:[NvPkg class]]) return;
    NvPkg *pk = holder;
    if (!zelda_texture_tracked(pk, t)) return;
    // A spell starts after Z_WRITEBACK_QUIET_FRAMES without GPU writes; writes in its first two frames are written back
    // (a texture's levels may be rendered across submissions).
    uint64_t frame = atomic_load(&g_cur_frame) + 1;
    if (!pk->z_written_frame || frame - pk->z_written_frame > Z_WRITEBACK_QUIET_FRAMES) pk->z_spell_frame = frame;
    pk->z_written_frame = frame;
    if (frame - pk->z_spell_frame <= 1 && !pk->z_writeback_queued) {
        if (!z_writeback_pending) z_writeback_pending = [NSMutableArray array];
        [z_writeback_pending addObject:@[pk, [NSData dataWithBytes:t length:sizeof *t]]];
        pk->z_writeback_queued = 1;
    }
    if (pk->z_rendered_pages) return;
    pk->z_rendered_pages = zelda_rendered_hashes(t);
    if (!z_rendered_marked++) MLOG("GPU-written upload texture: first ser=%llu %ux%u format=%#x; dirty-page hashes avoid uploads when guest bytes are unchanged", t->serial, t->d.width, t->d.height, t->d.format);
}
// Called instead of a re-upload when the texture's pages are dirty. Returns 1 when the GPU contents stay.
static int zelda_rendered_texture_unchanged(NvPkg *pk, const tex_t *t, int upload_lost) {
    if (!pk->z_rendered_pages || upload_lost || !zelda_texture_tracked(pk, t)) return 0;
    if (pk->z_writeback_fence) {   // a write-back changed the guest bytes: compare against them once it has landed
        if (!mtl_wait(pk->z_writeback_fence, 0)) return 1;
        pk->z_rendered_pages = zelda_rendered_hashes(t); pk->z_writeback_fence = 0;
    }
    int bad; const uint8_t *alias = zelda_texture_alias(pk, t, &bad);
    uint64_t since = pk->stamp;
    zelda_texture_protect(t);
    uint64_t stamp = nvn_dirty_now();
    NSData *hashes = pk->z_rendered_pages;
    const uint64_t *h = hashes.bytes;
    for (size_t i = 0, n = hashes.length / sizeof(uint64_t); i < n; i++) {
        uintptr_t a; size_t len = zelda_rendered_page(t, i, &a);
        // Writes through an alias mapping fault on the alias pages, so such a texture compares every page.
        if (!alias && !nvn_dirty_since((const void *)a, len, since)) continue;
        if (zelda_rendered_hash((const uint8_t *)a, len) != h[i]) {
            pk->z_rendered_pages = nil;
            if (!z_rendered_changed++) MLOG("GPU-written upload texture: first modified guest bytes ser=%llu; uploading guest contents", t->serial);
            return 0;
        }
    }
    // A guest write that raced with the comparison keeps the old stamp, so the next bind checks those pages again.
    if (!zelda_texture_dirty_since(pk, t, stamp)) pk->stamp = stamp;
    pk->seen = stamp; z_rendered_kept++;
    return 1;
}
