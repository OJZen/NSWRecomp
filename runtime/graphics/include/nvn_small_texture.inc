#include "nvn_texture_dirty.inc"
static void zelda_texture_snapshot_limit(NvPkg *package, const tex_t *texture, size_t limit) {
    package->small_bytes = nil;
    size_t size = texture->L.size;
    if (!size || size > limit || !zelda_texture_tracked(package, texture) ||
        zelda_texture_dirty_since(package, texture, package->stamp)) return;
    NSData *bytes = [NSData dataWithBytes:texture->mem length:size];
    if (!zelda_texture_dirty_since(package, texture, package->stamp)) package->small_bytes = bytes;
}
static void zelda_small_texture_snapshot(NvPkg *package, const tex_t *texture) {
    // Page neighbours can dirty tiny textures; bound the exact copy to one 16 KiB page of data.
    zelda_texture_snapshot_limit(package, texture, 16384);
}
// Textures that were re-uploaded once may keep a larger snapshot (ZELDA_TEXTURE_SNAPSHOT_MAX bytes), so a later dirty
// check can tell real content changes from writes to neighbouring data on the same pages.
static size_t zelda_reupload_snapshot_limit(void) {
    static size_t limit;
    if (!limit) { const char *value = getenv("ZELDA_TEXTURE_SNAPSHOT_MAX"); limit = value ? strtoull(value, NULL, 0) : 0; if (!limit) limit = 16384; }
    return limit;
}
// Only boundary pages can be dirtied by neighbours. Interior pages need no copy: a write there requires an upload.
static void zelda_texture_edges(const tex_t *texture, size_t *first, size_t *last) {
    size_t page = (size_t)getpagesize(), size = texture->L.size, offset = (uintptr_t)texture->mem % page;
    *first = offset ? MIN(page - offset, size) : 0;
    *last = size > *first ? ((uintptr_t)texture->mem + size) % page : 0;
}
static int z_texture_edge_reuse = -1;
static uint64_t z_texture_edges_created, z_texture_edges_kept;
static void zelda_reuploaded_texture_snapshot(NvPkg *package, const tex_t *texture) {
    zelda_texture_snapshot_limit(package, texture, zelda_reupload_snapshot_limit());
    if (z_texture_edge_reuse < 0) {
        const char *value = getenv("ZELDA_TEXTURE_EDGE_REUSE"); z_texture_edge_reuse = !value || atoi(value);
    }
    if (!z_texture_edge_reuse || package->small_bytes || !texture->L.size || !zelda_texture_tracked(package, texture) ||
        zelda_texture_dirty_since(package, texture, package->stamp)) return;
    size_t first, last; zelda_texture_edges(texture, &first, &last);
    if (!first && !last) return;
    NSMutableData *edges = [NSMutableData dataWithCapacity:first + last];
    if (first) [edges appendBytes:texture->mem length:first];
    if (last) [edges appendBytes:(const uint8_t *)texture->mem + texture->L.size - last length:last];
    if (!zelda_texture_dirty_since(package, texture, package->stamp)) {
        package->small_bytes = edges; z_texture_edges_created++;
    }
}
static int zelda_small_texture_unchanged(NvPkg *package, const tex_t *texture, int upload_lost) {
    if (upload_lost || !package->small_bytes || !zelda_texture_tracked(package, texture)) return 0;
    zelda_texture_protect(texture);
    uint64_t stamp = nvn_dirty_now();
    if (package->small_bytes.length == texture->L.size) {
        if (memcmp(package->small_bytes.bytes, texture->mem, texture->L.size)) return 0;
    } else {
        size_t first, last; zelda_texture_edges(texture, &first, &last);
        if (package->small_bytes.length != first + last || first + last > texture->L.size) return 0;
        size_t middle = texture->L.size - first - last;
        int bad; const uint8_t *alias = zelda_texture_alias(package, texture, &bad);
        if (bad || (middle && (nvn_dirty_since((const uint8_t *)texture->mem + first, middle, package->stamp) ||
            (alias && nvn_dirty_since(alias + first, middle, package->stamp))))) return 0;
        const uint8_t *bytes = package->small_bytes.bytes;
        if ((first && memcmp(bytes, texture->mem, first)) ||
            (last && memcmp(bytes + first, (const uint8_t *)texture->mem + texture->L.size - last, last))) return 0;
    }
    if (zelda_texture_dirty_since(package, texture, stamp)) return 0;
    if (package->small_bytes.length != texture->L.size) z_texture_edges_kept++;
    package->stamp = package->seen = stamp;
    return 1;
}
