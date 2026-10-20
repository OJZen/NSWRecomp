// Included after the shared interface so additions use its object layouts and signature checks.
#include "nvn_buffer.inc"
#include "nvn_write.inc"
#include "nvn_state.inc"
#include "nvn_deferred.inc"
#include "nvn_conditional.inc"
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int count, const NVNcommandHandle *handles) {
    // The shared recorder's CopyCommands already emits OP_CALL references, with return-stack replay.
    h_nvnCommandBufferCopyCommands(c, count, handles);
}
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int index, NVNbufferAddress address, size_t size) {
    const NVNbufferRange range = {address, size};
    h_nvnCommandBufferBindVertexBuffers(c, index, 1, &range);
}
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int width, int height) {
    const float viewport[] = {x, y, width, height};
    h_nvnCommandBufferSetViewports(c, 0, 1, viewport);
}
// Debug labels do not affect execution and are intentionally not copied from guest memory.
#define Z_LABEL(name, type) NVN(name, void, type *o, const char *label) { (void)o; (void)label; }
Z_LABEL(nvnDeviceSetDebugLabel, NVNdevice)
Z_LABEL(nvnQueueSetDebugLabel, NVNqueue)
Z_LABEL(nvnWindowSetDebugLabel, NVNwindow)
Z_LABEL(nvnProgramSetDebugLabel, NVNprogram)
Z_LABEL(nvnMemoryPoolSetDebugLabel, NVNmemoryPool)
Z_LABEL(nvnTexturePoolSetDebugLabel, NVNtexturePool)
Z_LABEL(nvnSamplerPoolSetDebugLabel, NVNsamplerPool)
Z_LABEL(nvnBufferSetDebugLabel, NVNbuffer)
Z_LABEL(nvnTextureSetDebugLabel, NVNtexture)
Z_LABEL(nvnSamplerSetDebugLabel, NVNsampler)
Z_LABEL(nvnCommandBufferSetDebugLabel, NVNcommandBuffer)
Z_LABEL(nvnSyncSetDebugLabel, NVNsync)
#undef Z_LABEL
typedef struct { NVNshaderData shader; const void *metadata; } ZeldaShaderDataExt;
_Static_assert(sizeof(ZeldaShaderDataExt) == 24, "ShaderDataExt stride at main+0x72fea4");
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int count, const ZeldaShaderDataExt *data) {
    if (count < 0 || count > 6 || (count && !data)) return 0;
    NVNshaderData shaders[6];
    for (int i = 0; i < count; i++) {
        // shortcut: Zelda currently supplies no extension metadata; inspect non-null extensions before using them.
        if (data[i].metadata) { LOG("BLOCKED NVN shader extension metadata"); exit(78); }
        shaders[i] = data[i].shader;
    }
    return h_nvnProgramSetShaders(p, count, shaders);
}
// Control records live in the shared command stream; the separate control arena is unused.
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->control_size; }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->control_size; }
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) {
    const cmd_t *o = (const cmd_t *)c;
    return o->guest_active ? (size_t)(o->cur - o->guest_memory) + (o->command_callback_pending ? 16 : 0) : o->guest_used;
}
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) {
    return ((const cmd_t *)c)->guest_size;
}
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) {
    return h_nvnCommandBufferGetCommandMemorySize(c) - h_nvnCommandBufferGetCommandMemoryUsed(c);
}
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    int first, int count, int base_instance, int instances) {
    if (first < 0 || count < 0 || base_instance < 0 || instances < 0) die("NVN array draw range");
    *REC(c, OP_DRAW, p_draw) = (p_draw){primitive, first, count, base_instance, instances};
}
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices) {
    h_nvnCommandBufferDrawElementsInstanced(c, primitive, type, count, indices, 0, 0, 1);
}
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices, int base_vertex) {
    h_nvnCommandBufferDrawElementsInstanced(c, primitive, type, count, indices, base_vertex, 0, 1);
}
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int texture, int sampler) {
    (void)d;
    if ((unsigned)texture > 0xfffff || (unsigned)sampler > 0xfff) { LOG("BLOCKED NVN combined handle range"); exit(78); }
    return (uint64_t)texture | (uint64_t)sampler << 20;
}
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage stage, int index, NVNtextureHandle handle) {
    bind_handles(c, OP_BIND_TEX, stage, index, 1, &handle);
}
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage stage, int first, int count, const NVNtextureHandle *handles) {
    bind_handles(c, OP_BIND_TEX, stage, first, count, handles);
}
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return ((const pool_t *)p)->mem; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return ((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return ((const pool_t *)p)->size; }
// The host converts Tegra layouts at upload, so all texture storage shares one host class.
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return 1; }
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return 1; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return ((const tex_t *)t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return ((const tex_t *)t)->d.target; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return ((const tex_t *)t)->d.samples; }
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return ((const tex_t *)t)->d.levels; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return ((const tex_t *)t)->d.stride; }
// Metal manages hierarchical depth internally and needs no guest ZCull save area.
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { (void)b; return 0; }
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) {
    (void)t; (void)v; (void)r; atomic_thread_fence(memory_order_seq_cst);
}
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *builder, NVNsamplerReduction mode) {
    (void)builder;
    // shortcut: ordinary weighted sampling is supported; min/max reduction needs a Metal sampler variant.
    if (mode) { LOG("BLOCKED NVN sampler reduction mode=%d", mode); exit(78); }
}
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode mode) {
    (void)b;
    // shortcut: depth sampling is the backend default; add stencil-only views when first requested.
    if (mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) { LOG("BLOCKED NVN stencil sampling"); exit(78); }
}
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode mode) {
    (void)v;
    if (mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) { LOG("BLOCKED NVN stencil sampling view"); exit(78); }
}
static uint16_t texture_swizzle(NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    if ((unsigned)r > 5 || (unsigned)g > 5 || (unsigned)blue > 5 || (unsigned)a > 5) {
        LOG("BLOCKED NVN invalid component swizzle"); exit(78);
    }
    return 0x8000 | r | (g << 3) | (blue << 6) | (a << 9);
}
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g,
    NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    ((tex_builder_t *)b)->swizzle = texture_swizzle(r, g, blue, a);
}
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g,
    NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    ((view_t *)v)->swizzle = texture_swizzle(r, g, blue, a);
}
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g,
    NVNtextureSwizzle *blue, NVNtextureSwizzle *a) {
    uint16_t s = ((const tex_t *)t)->swizzle;
    if (!(s & 0x8000)) s = texture_swizzle(NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A);
    *r = s & 7; *g = (s >> 3) & 7; *blue = (s >> 6) & 7; *a = (s >> 9) & 7;
}
// NVN's optional compression palette is unavailable; ordinary Metal clears remain supported.
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *v, NVNformat f) { (void)d; (void)v; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *d, const int *v, NVNformat f) { (void)d; (void)v; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *d, const uint32_t *v, NVNformat f) { (void)d; (void)v; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float v) { (void)d; (void)v; return 0; }
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *builder, int width, int height) {
    h_nvnTextureBuilderSetWidth(builder, width);
    h_nvnTextureBuilderSetHeight(builder, height);
    h_nvnTextureBuilderSetDepth(builder, 1);
}

NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *builder, int count) {
    // shortcut: the shared present queue uses every image; stop if a game requests a smaller active subset.
    if (count != ((const win_builder_t *)builder)->ntex) {
        LOG("BLOCKED NVN window active subset count=%d", count); exit(78);
    }
}
