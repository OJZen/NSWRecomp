// State belongs to one encoder, including helper clears; reset even when Metal reuses its object address.
static struct {
    unsigned valid;
    MTLViewport viewport;
    MTLScissorRect scissor;
    MTLCullMode cull;
    MTLWinding winding;
    MTLTriangleFillMode fill;
    BOOL clip;
    uint32_t stencil;
    float bias[3], blend[4];
    uint32_t buffers_valid[2];
    uint64_t buffers[2][31];
} z_encoder_state;
static uint64_t z_encoder_state_sent, z_encoder_state_reused;
static int z_encoder_state_enabled = -1;
static void zelda_encoder_state_reset(void) {
    if (z_encoder_state_enabled < 0) {
        const char *value = getenv("ZELDA_ENCODER_STATE_CACHE");
        z_encoder_state_enabled = !value || atoi(value);
    }
    z_encoder_state.valid = 0;
    z_encoder_state.buffers_valid[0] = z_encoder_state.buffers_valid[1] = 0;
}
static inline void zelda_encoder_buffer(id<MTL4ArgumentTable> table, unsigned stage, NSUInteger index, uint64_t address) {
    if (stage < 2 && index < 31) {
        uint32_t bit = 1u << index;
        if (z_encoder_state_enabled && (z_encoder_state.buffers_valid[stage] & bit) &&
            z_encoder_state.buffers[stage][index] == address) { z_encoder_state_reused++; return; }
        z_encoder_state.buffers_valid[stage] |= bit; z_encoder_state.buffers[stage][index] = address;
    }
    z_encoder_state_sent++; [table setAddress:address atIndex:index];
}
static inline int zelda_encoder_state_same(unsigned bit, void *old, const void *value, size_t size) {
    if (z_encoder_state_enabled && (z_encoder_state.valid & bit) && !memcmp(old, value, size)) {
        z_encoder_state_reused++; return 1;
    }
    memcpy(old, value, size); z_encoder_state.valid |= bit; z_encoder_state_sent++;
    return 0;
}
#define Z_STATE_SAME(bit, member, value) zelda_encoder_state_same(1u << (bit), &z_encoder_state.member, &(value), sizeof(value))
static inline void enc_r_viewport(RENC e, MTLViewport v) {
    if (!Z_STATE_SAME(0, viewport, v)) [e setViewport:v];
}
static inline void enc_r_scissor(RENC e, MTLScissorRect r) {
    if (!Z_STATE_SAME(1, scissor, r)) [e setScissorRect:r];
}
static inline void enc_r_cull(RENC e, MTLCullMode m) {
    if (!Z_STATE_SAME(2, cull, m)) [e setCullMode:m];
}
static inline void enc_r_winding(RENC e, MTLWinding w) {
    if (!Z_STATE_SAME(3, winding, w)) [e setFrontFacingWinding:w];
}
static inline void enc_r_fill(RENC e, MTLTriangleFillMode m) {
    if (!Z_STATE_SAME(4, fill, m)) [e setTriangleFillMode:m];
}
static inline void enc_r_depth_bias(RENC e, float u, float s, float c) {
    float value[] = {u, s, c};
    if (!Z_STATE_SAME(5, bias, value)) [e setDepthBias:u slopeScale:s clamp:c];
}
static inline void enc_r_depth_clip(RENC e, BOOL v) {
    if (!Z_STATE_SAME(6, clip, v)) [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
}
static inline void enc_r_stencil_ref(RENC e, uint32_t f, uint32_t b) {
    (void)b;
    if (!Z_STATE_SAME(7, stencil, f)) [e setStencilReferenceValue:f];
}
static inline void enc_r_blend_color(RENC e, float r, float g, float b, float a) {
    float value[] = {r, g, b, a};
    if (!Z_STATE_SAME(8, blend, value)) [e setBlendColorRed:r green:g blue:b alpha:a];
}
#undef Z_STATE_SAME
