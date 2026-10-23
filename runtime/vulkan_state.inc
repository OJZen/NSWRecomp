// Adapted from the locally maintained Zelda-Mobile NVN state setters.
// MIT; see third_party/zelda-mobile/d2r/LICENSE and SOURCE.json.
#define ST(T, p) WRITE(T,p)
#define B(x) ((x)&255)
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *state,NVNlogicOp op) {ST(color_st,state)->logic_op=op;}
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *state,NVNalphaFunc func) {ST(color_st,state)->alpha_func=func;}
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) die("invalid NVN state target");
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) die("invalid NVN state target");
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(WRITE(ms_st,s), 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) die("invalid NVN vertex offset");
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }

#undef B
#undef ST
