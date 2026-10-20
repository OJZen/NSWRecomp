#include "shader_status.h"
// Write-back of GPU-written packaged textures into guest memory at the end of the submission that wrote them (policy in
// nvn_texture_rendered.inc). Each level / layer is copied into a scratch buffer, then a kernel scatters the rows into the
// guest layout in the pool buffer (pitch-linear textures are copied straight into the pool). Only formats whose Metal
// bytes are the guest bytes take part: converted or compressed formats and depth / stencil keep the old behaviour.
#include <TargetConditionals.h>
static const char *zelda_writeback_msl = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ZwbParams { device uchar *dst; uint row_bytes, rows, slices, tile_h, tile_d, tight_row; };
static ulong zwb_gob(uint x, uint y) { return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16); }
// One thread per 16-byte run of a row: block-linear storage keeps those runs contiguous.
kernel void zwb_scatter(device const uchar *src [[buffer(0)]], constant ZwbParams &p [[buffer(1)]], uint3 id [[thread_position_in_grid]]) {
    uint x = id.x * 16, y = id.y, z = id.z;
    if (x >= p.row_bytes || y >= p.rows || z >= p.slices) return;
    uint bh = 1u << p.tile_h, bd = 1u << p.tile_d;
    ulong gobs_x = (p.row_bytes + 63) / 64, blocks_y = (p.rows + 8 * bh - 1) / (8 * bh), block = 512ul * bh * bd;
    ulong at = (z / bd) * gobs_x * blocks_y * block + (y / (8 * bh)) * gobs_x * block + (x / 64) * block
             + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ul + zwb_gob(x, y);
    const device uchar *s = src + ((ulong)z * p.rows + y) * p.tight_row + x;
    for (uint i = 0, n = min(16u, p.row_bytes - x); i < n; i++) p.dst[at + i] = s[i];
}
)MSL";
typedef struct { uint64_t dst; uint32_t row_bytes, rows, slices, tile_h, tile_d, tight_row; } zwb_params_t;
#ifndef ZELDA_WRITEBACK_ANALYSIS_ONLY
static uint64_t z_writeback_textures, z_writeback_bytes, z_writeback_skipped;

static int zelda_writeback_format(const tex_t *t, id<MTLTexture> texture) {
    uint32_t pf = 0; uint8_t swz[4];
    if (fmt_compressed(t->d.format) || nvn_fmt_to_mtl(t->d.format, &pf, swz) != 1 || pf != (uint32_t)texture.pixelFormat) return 0;
    switch (texture.pixelFormat) {
        case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatStencil8:
#if !TARGET_OS_IPHONE
        case MTLPixelFormatDepth24Unorm_Stencil8: case MTLPixelFormatX24_Stencil8:
#endif
        case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8: return 0;
        default: return 1;
    }
}
static id<MTLComputePipelineState> zelda_writeback_pipeline(id<MTL4ArgumentTable> *table) {
    static id<MTLComputePipelineState> pipeline; static id<MTL4ArgumentTable> arguments; static int failed;
    if (!pipeline && !failed) {
        NSError *error = nil; id<MTLFunction> f = [ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:@(zelda_writeback_msl) options:nil error:&error]) newFunctionWithName:@"zwb_scatter"];
        pipeline = f ? ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:f error:&error]) : nil;
        MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
        arguments = pipeline ? [g_dev newArgumentTableWithDescriptor:d error:&error] : nil;
        if (!arguments) { failed = 1; MLOG("ERROR: Texture writeback kernel: %s", error.localizedDescription.UTF8String); }
    }
    *table = arguments;
    return arguments ? pipeline : nil;
}
// One texture: copies first (all levels / layers), then the scatter kernels behind one barrier.
static int zelda_writeback_texture(NvPkg *pk, const tex_t *t) {
    id<MTLTexture> texture = pk->tex;
    if (!texture || !zelda_writeback_format(t, texture) || t->d.samples > 1 || t->d.target == 2 || t->d.target == 10) return 0;
    void *pool; uint64_t pool_offset, pool_gpu;
    if (!mtl_gpu_addr((uint64_t)(uintptr_t)t->mem, t->L.size, &pool, &pool_offset, &pool_gpu)) return 0;
    id<MTLBuffer> memory = (__bridge id<MTLBuffer>)pool;   // looking the address up made the pool resident
    if (t->d.flags & 0x50) {   // pitch-linear: rows go straight to the pool
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (t->d.target != 1 || t->d.levels > 1 || bw != 1 || bh != 1 || !t->d.height) return 0;
        size_t stride = t->L.size / t->d.height;
        if (stride < (size_t)t->d.width * bpb) return 0;
        enc_b_copy_texture_buffer(blit(), texture, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t->d.width, t->d.height, 1), memory, pool_offset, stride, stride * t->d.height);
        z_writeback_bytes += t->L.size;
        return 1;
    }
    id<MTL4ArgumentTable> table; id<MTLComputePipelineState> pipeline = zelda_writeback_pipeline(&table);
    if (!pipeline) return 0;
    unsigned levels = t->d.levels ? t->d.levels : 1, layers = t->L.layers ? t->L.layers : 1;
    if (texture.mipmapLevelCount < levels || (texture.textureType == MTLTextureType2DArray ? texture.arrayLength : texture.textureType == MTLTextureTypeCube ? 6 : 1) < layers) return 0;
    size_t total = 0;
    for (unsigned level = 0; level < levels; level++) {
        uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&t->d, level, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return 0;
        total += (size_t)row_bytes * rows * slices * layers;
    }
    id<MTLBuffer> scratch = [g_dev newBufferWithLength:total options:MTLResourceStorageModePrivate];
    if (!scratch) return 0;
    res_add(scratch);
    BENC b = blit();
    size_t at = 0;
    for (unsigned level = 0; level < levels; level++) for (unsigned layer = 0; layer < layers; layer++) {
        uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb);
        enc_b_copy_texture_buffer(b, texture, layer, level, MTLOriginMake(0, 0, 0), MTLSizeMake(row_bytes / bpb, rows, slices), scratch, at, row_bytes, (size_t)row_bytes * rows);
        at += (size_t)row_bytes * rows * slices;
    }
    zelda_blit_barrier();
    at = 0;
    for (unsigned level = 0; level < levels; level++) for (unsigned layer = 0; layer < layers; layer++) {
        uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb);
        zwb_params_t p = {pool_gpu + base, row_bytes, rows, slices, th, td, row_bytes};
        id<MTLBuffer> arena; size_t o = arena_alloc(sizeof p, &arena); memcpy((uint8_t *)arena.contents + o, &p, sizeof p);
        [table setAddress:scratch.gpuAddress + at atIndex:0];
        [table setAddress:arena.gpuAddress + o atIndex:1];
        [(id<MTL4ComputeCommandEncoder>)b setArgumentTable:table];
        [(id<MTL4ComputeCommandEncoder>)b setComputePipelineState:pipeline];
        [(id<MTL4ComputeCommandEncoder>)b dispatchThreads:MTLSizeMake((row_bytes + 15) / 16, rows, slices) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
        at += (size_t)row_bytes * rows * slices;
    }
    defer_obj_release(scratch);
    z_writeback_bytes += total;
    return 1;
}
// Called at the end of every submission, after the last render pass.
static void zelda_rendered_writeback(void) {
    if (!z_writeback_pending.count) return;
    NSArray *pending = z_writeback_pending; z_writeback_pending = nil;
    uint64_t fence = g_fence + 1;   // this submission's fence (mtl_sub_commit takes it right after)
    for (NSArray *entry in pending) {
        NvPkg *pk = entry[0]; const tex_t *t = [entry[1] bytes];
        pk->z_writeback_queued = 0;
        if (zelda_writeback_texture(pk, t)) {
            pk->z_writeback_fence = fence;
            if (!z_writeback_textures++) MLOG("GPU-written upload texture written back to guest memory: first ser=%llu %ux%u format=%#x levels=%u", t->serial, t->d.width, t->d.height, t->d.format, t->d.levels);
        } else z_writeback_skipped++;
    }
    zelda_blit_barrier();
    static uint64_t logged;   // cumulative counts at powers of two, so the steady-state rate shows in the log
    if (z_writeback_textures >= 2 * logged) {
        logged = MAX(z_writeback_textures, 1);
        MLOG("Texture writeback total=%llu textures %.1f MiB skipped=%llu frame=%llu", z_writeback_textures, z_writeback_bytes / 1048576.0, z_writeback_skipped,
             (unsigned long long)atomic_load(&g_cur_frame));
    }
}
#endif
