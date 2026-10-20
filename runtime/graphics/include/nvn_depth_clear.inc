#include "shader_status.h"
static void zelda_depth_clear_encode(void) {
    if (!z_depth_clear.pending) return;
    z_depth_clear.pending = 0;
    if (!g_enc || !g_pass.hd) MDIE("Conditional depth clear lost its attachment");
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
    static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *states;
    if (!pipelines) { pipelines = [NSMutableDictionary dictionary]; states = [NSMutableDictionary dictionary]; }
    uint32_t formats[10]; memcpy(formats, g_pass.cfmt, 8 * sizeof(uint32_t));
    formats[8] = g_pass.dfmt; formats[9] = g_pass.sfmt;
    NSData *key = [NSData dataWithBytes:formats length:sizeof formats];
    id<MTLRenderPipelineState> pipeline = pipelines[key];
    if (!pipeline) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [z_query_library newFunctionWithName:@"depth_clear_vertex"];
        for (int i = 0; i < 8; i++) { d.colorAttachments[i].pixelFormat = formats[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }
        d.depthAttachmentPixelFormat = formats[8]; d.stencilAttachmentPixelFormat = formats[9];
        NSError *error = nil; pipeline = ZELDA_SHADER_BUILD([g_dev newRenderPipelineStateWithDescriptor:d error:&error]);
        if (!pipeline) MDIE("Inline depth clear pipeline failed: %s", error.localizedDescription.UTF8String);
        pipelines[key] = pipeline;
    }
    unsigned stencil = g_pass.hs ? z_depth_clear.clear.stencil_mask & 255 : 0;
    NSNumber *state_key = @((!!z_depth_clear.clear.depth_mask) | (stencil << 1));
    id<MTLDepthStencilState> state = states[state_key];
    if (!state) {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = !!z_depth_clear.clear.depth_mask;
        if (stencil) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = MTLCompareFunctionAlways; s.depthStencilPassOperation = MTLStencilOperationReplace;
            s.writeMask = stencil; d.frontFaceStencil = s; d.backFaceStencil = s;
        }
        state = [g_dev newDepthStencilStateWithDescriptor:d];
        if (!state) MDIE("Inline depth clear state creation failed");
        states[state_key] = state;
    }
    long width = g_pass.tw, height = g_pass.th, x = 0, y = 0, right = width, bottom = height;
    if (z_depth_clear.has_scissor) {
        p_scissor s = z_depth_clear.scissor;
        x = MAX(s.x, 0); y = MAX(s.y, 0);
        right = MIN((int64_t)s.x + s.w, width); bottom = MIN((int64_t)s.y + s.h, height);
    }
    if (right <= x || bottom <= y) return;
    [z_query_args setAddress:z_depth_clear.query atIndex:0];
    [z_query_args setAddress:z_depth_clear.parameters atIndex:1];
    [g_enc setArgumentTable:z_query_args atStages:MTLRenderStageVertex];
    enc_r_pso(g_enc, pipeline); enc_r_dss(g_enc, state);
    enc_r_viewport(g_enc, (MTLViewport){0, 0, width, height, 0, 1});
    enc_r_scissor(g_enc, (MTLScissorRect){x, y, right - x, bottom - y});
    enc_r_cull(g_enc, MTLCullModeNone); enc_r_fill(g_enc, MTLTriangleFillModeFill);
    enc_r_depth_bias(g_enc, 0, 0, 0); enc_r_depth_clip(g_enc, YES);
    enc_r_stencil_ref(g_enc, z_depth_clear.clear.stencil, z_depth_clear.clear.stencil);
    enc_r_visibility(g_enc, MTLVisibilityResultModeDisabled, 0);
    [g_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // A later draw sampling this depth/stencil storage must first end the pass and store the clear.
    if (z_depth_clear.clear.depth_mask || stencil) z_pass_depth_written = 1;
    for (int i = 0; i < g_nsd; i++) if ((g_sd[i].depth == 1 && z_depth_clear.clear.depth_mask) ||
        (g_sd[i].depth == 2 && stencil)) g_sd[i].wrote = 1;
    [g_enc setArgumentTable:g_vt atStages:MTLRenderStageVertex];
    [g_enc setArgumentTable:g_ft atStages:MTLRenderStageFragment];
    g_enc_pso = nil; g_enc_dss = nil; g_enc_vis = 0;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
}
