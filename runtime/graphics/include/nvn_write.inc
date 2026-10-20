#include "tex/nvn_deswizzle.h"

static void texture_write(const NVNtexture *texture, const NVNtextureView *view, const NVNcopyRegion *region,
                          const void *source, ptrdiff_t row_stride, ptrdiff_t image_stride) {
    const tex_t *t = (const tex_t *)texture;
    const view_t *v = (const view_t *)view;
    if (region && region->width >= 0 && region->height >= 0 && region->depth >= 0 &&
        (!region->width || !region->height || !region->depth)) return;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0;
    unsigned layer = v && v->set_layers ? (unsigned)v->min_layer : 0;
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (!source || !t->mem || !region || level >= 32 || nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) {
        LOG("BLOCKED NVN texture CPU write geometry"); exit(78);
    }
    unsigned width = t->d.width >> level, height = t->d.height >> level;
    if (!width) width = 1; if (!height) height = 1;
    int bw, bh; nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned available = t->d.target == 2 ? slices : t->L.layers - layer;
    if (v && v->set_layers && v->nlayers > 0 && (unsigned)v->nlayers < available) available = (unsigned)v->nlayers;
    if (region->xoffset < 0 || region->yoffset < 0 || region->zoffset < 0 ||
        region->width <= 0 || region->height <= 0 || region->depth <= 0 ||
        (uint64_t)region->xoffset + region->width > width || (uint64_t)region->yoffset + region->height > height ||
        (uint64_t)region->zoffset + region->depth > available || region->xoffset % bw || region->yoffset % bh ||
        (v && v->set_format && v->format != t->d.format)) {
        LOG("BLOCKED NVN texture write target=%d size=%ux%ux%u region=%d,%d,%d+%d,%d,%d format=%x view=%x",
            t->d.target, width, height, available, region->xoffset, region->yoffset, region->zoffset,
            region->width, region->height, region->depth, t->d.format, v && v->set_format ? v->format : t->d.format); exit(78);
    }
    // Compressed data comes in whole blocks: a region ending inside a block row/column (v1.4.3 movie subtitles write
    // BC4 strips 126 rows high mid-texture) still writes that whole block, which stays inside the block grid.
    size_t write_bytes = ((unsigned)region->width + bw - 1) / bw * bpb;
    size_t write_rows = ((unsigned)region->height + bh - 1) / bh;
    if (!row_stride) row_stride = (ptrdiff_t)write_bytes;
    if (row_stride < (ptrdiff_t)write_bytes || (uint64_t)row_stride > PTRDIFF_MAX / write_rows) {
        LOG("BLOCKED NVN texture row stride"); exit(78);
    }
    if (!image_stride) image_stride = (ptrdiff_t)write_rows * row_stride;
    if (image_stride < (ptrdiff_t)write_rows * row_stride || (uint64_t)image_stride > PTRDIFF_MAX / (unsigned)region->depth) {
        LOG("BLOCKED NVN texture image stride"); exit(78);
    }
    dm_guest_write_prepare(t->mem, t->L.size);
    for (unsigned z = 0; z < (unsigned)region->depth; z++) {
        unsigned dst_layer = t->d.target == 2 ? layer : layer + region->zoffset + z;
        unsigned dst_z = t->d.target == 2 ? region->zoffset + z : 0;
        if (nvn_deswizzle_geom(&t->d, level, dst_layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) abort();
        for (size_t y = 0; y < write_rows; y++) for (size_t x = 0; x < write_bytes;) {
            unsigned dst_x = (unsigned)region->xoffset / bw * bpb + (unsigned)x;
            size_t count = 16 - (dst_x & 15);
            if (count > write_bytes - x) count = write_bytes - x;
            uint64_t offset = base + nvn_bl_offset(dst_x, (unsigned)region->yoffset / bh + (unsigned)y, dst_z, row_bytes, rows, th, td);
            if (offset > t->L.size || count > t->L.size - offset) abort();
            memcpy(t->mem + offset, (const uint8_t *)source + z * image_stride + y * row_stride + x, count);
            x += count;
        }
    }
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *texture, const NVNtextureView *view,
    const NVNcopyRegion *region, const void *data) { texture_write(texture, view, region, data, 0, 0); }
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *texture, const NVNtextureView *view,
    const NVNcopyRegion *region, const void *data, ptrdiff_t row, ptrdiff_t image) { texture_write(texture, view, region, data, row, image); }
