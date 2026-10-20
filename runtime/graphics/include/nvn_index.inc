#include "shader_status.h"
#include "nvn_index_msl.h"
static uint64_t z_index_hits, z_index_expansions;
static void zelda_blit_barrier(void) {
    [g_blit barrierAfterEncoderStages:(MTLStageDispatch | MTLStageBlit)
        beforeEncoderStages:(MTLStageDispatch | MTLStageBlit) visibilityOptions:MTL4VisibilityOptionDevice];
}
// Rasterization-free vertex pipelines that write buffers inside the open render pass, keyed by its attachment formats
// and the function (constant updates in nvn_pass_writes.inc, index conversions below).
static id<MTLRenderPipelineState> zelda_vertex_write_pipeline(id<MTLFunction> function) {
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
    if (!pipelines) pipelines = [NSMutableDictionary dictionary];
    uint32_t key[11]; memcpy(key, g_pass.cfmt, 8 * sizeof(uint32_t));
    key[8] = g_pass.dfmt; key[9] = g_pass.sfmt; key[10] = MAX(g_pass.samples, 1u);
    NSMutableData *name = [NSMutableData dataWithBytes:key length:sizeof key];
    [name appendData:[function.name dataUsingEncoding:NSUTF8StringEncoding]];
    id<MTLRenderPipelineState> pipeline = pipelines[name];
    if (!pipeline) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = function; d.rasterizationEnabled = NO;
        for (int i = 0; i < 8; i++) { d.colorAttachments[i].pixelFormat = key[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }
        d.depthAttachmentPixelFormat = key[8]; d.stencilAttachmentPixelFormat = key[9]; d.rasterSampleCount = key[10];
        NSError *error = nil; pipeline = ZELDA_SHADER_BUILD([g_dev newRenderPipelineStateWithDescriptor:d error:&error]);
        if (!pipeline) MDIE("Pass buffer-write pipeline %s failed: %s", function.name.UTF8String, error.localizedDescription.UTF8String);
        pipelines[name] = pipeline;
    }
    return pipeline;
}
// Conversions waiting for the render pass of the draw that needs them: drawn there instead of in a copy encoder
// (which costs a copy -> render transition, or breaks an open pass). ZELDA_INDEX_IN_PASS=0 keeps the copy encoder.
typedef struct { uint64_t source, target; uint32_t count; } zelda_index_job_t;
static zelda_index_job_t z_index_jobs[8];
static int z_index_job_n;
static uint64_t z_index_in_pass;
static id<MTLComputePipelineState> z_index_pipeline;
static id<MTL4ArgumentTable> z_index_arguments;
static id<MTLFunction> z_index_vertex;
static void zelda_index_compute(const zelda_index_job_t *job) {
    // Convert on the queue so earlier GPU writes are visible before indexed drawing.
    id<MTL4ComputeCommandEncoder> encoder = blit();
    zelda_blit_barrier();
    [encoder setArgumentTable:z_index_arguments];
    [z_index_arguments setAddress:job->source atIndex:0]; [z_index_arguments setAddress:job->target atIndex:1];
    [z_index_arguments setAddress:job->target - 256 atIndex:2];
    enc_c_pso(encoder, z_index_pipeline);
    enc_c_dispatch(encoder, MTLSizeMake((job->count + 127) / 128, 1, 1), MTLSizeMake(128, 1, 1));
    zelda_blit_barrier();
    args_dirty_all();
}
// Right after the draw's pass is open (or found open): convert, then an in-pass vertex barrier orders the index fetch.
static void zelda_index_encode(id pass) {
    if (!z_index_job_n) return;
    int count = z_index_job_n; z_index_job_n = 0;
    if (!pass || pass != g_enc) { for (int i = 0; i < count; i++) zelda_index_compute(&z_index_jobs[i]); return; }
    enc_r_pso(g_enc, zelda_vertex_write_pipeline(z_index_vertex));
    for (int i = 0; i < count; i++) {
        [z_index_arguments setAddress:z_index_jobs[i].source atIndex:0]; [z_index_arguments setAddress:z_index_jobs[i].target atIndex:1];
        [g_enc setArgumentTable:z_index_arguments atStages:MTLRenderStageVertex];
        [g_enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:z_index_jobs[i].count];
    }
    [g_enc barrierAfterEncoderStages:MTLStageVertex beforeEncoderStages:MTLStageVertex visibilityOptions:MTL4VisibilityOptionDevice];
    [g_enc setArgumentTable:g_vt atStages:MTLRenderStageVertex];
    g_enc_pso = nil; args_dirty_all(); g_pso_dirty = 1;
    z_index_in_pass += count;
}
static uint64_t zelda_expand_indices(const p_draw_idx *draw) {
    id<MTLComputePipelineState> pipeline;
    id<MTL4ArgumentTable> arguments;
    // shortcut: the shared arena is 4 MiB; chunk larger index buffers if a game requests them.
    if (draw->count <= 0 || draw->count > ((4 << 20) - 256) / 2) MDIE("8-bit index count exceeds arena bounds");
    void *source; uint64_t offset, address;
    if (!mtl_gpu_addr(draw->ibuf, draw->count, &source, &offset, &address)) MDIE("8-bit indices outside memory pool");
    zelda_buffer_cache_sync();
    uint64_t canonical = nvn_canonical_address(draw->ibuf, draw->count);
    for (unsigned i = 0; canonical && i < z_index_count; i++)
        if (z_index_cache[i].source == canonical && z_index_cache[i].length == (uint64_t)draw->count)
            { z_index_hits++; return z_index_cache[i].expanded; }
    z_index_expansions++;
    PD("I expand address=%#llx count=%d", (unsigned long long)canonical, draw->count);
    if (!z_index_pipeline) {
        NSError *error = nil;
        id<MTLLibrary> library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:@(zelda_index_msl) options:nil error:&error]);
        id<MTLFunction> function = [library newFunctionWithName:@"expand_index"];
        if (function) z_index_pipeline = ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:function error:&error]);
        z_index_vertex = [library newFunctionWithName:@"expand_index_vertex"];
        MTL4ArgumentTableDescriptor *descriptor = [MTL4ArgumentTableDescriptor new]; descriptor.maxBufferBindCount = 3;
        z_index_arguments = [g_dev newArgumentTableWithDescriptor:descriptor error:&error];
        if (!z_index_pipeline || !z_index_arguments || !z_index_vertex) MDIE("8-bit index conversion pipeline could not be created");
    }
    pipeline = z_index_pipeline; arguments = z_index_arguments;
    id<MTLBuffer> expanded = nil;
    // shortcut: at most 64 * (8 KiB + 256 B) owned results; larger draws keep the arena path.
    if (canonical && zelda_index_reuse_enabled() && draw->count <= 4096 && z_index_count < 64) {
        size_t bytes = ((size_t)draw->count * 2 + 511) & ~(size_t)255;
        expanded = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!expanded) MDIE("Index cache allocation failed");
        res_add(expanded);
    }
    BOOL owned = expanded != nil;
    size_t start = owned ? 0 : arena_alloc((size_t)draw->count * 2 + 256, &expanded);
    uint32_t count = draw->count; memcpy((uint8_t *)expanded.contents + start, &count, 4);
    uint64_t indices = expanded.gpuAddress + start + 256;
    (void)pipeline; (void)arguments;
    static int in_pass = -1;
    if (in_pass < 0) { const char *value = getenv("ZELDA_INDEX_IN_PASS"); in_pass = !value || atoi(value); }
    zelda_index_job_t job = {address, indices, count};
    if (in_pass && !tbdr_on() && z_index_job_n < 8) z_index_jobs[z_index_job_n++] = job;   // drawn by zelda_index_encode
    else zelda_index_compute(&job);
    // Changed sources get a new result; old GPU readers retain the previous storage through their fence.
    if (canonical && z_index_count < 64)
        z_index_cache[z_index_count++] = (typeof(z_index_cache[0])){canonical, (uint64_t)draw->count, indices, owned ? expanded : nil};
    return indices;
}
