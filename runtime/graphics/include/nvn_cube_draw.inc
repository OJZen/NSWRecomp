static NvProg *zelda_cube_program(NvProg *program) {
    if (program->z_cube) return program->z_cube;
    NSString *key = program->sh[0]->z_cube_uniform == 1 ? @"48e3901f9b4eb169_b" : @"c3960b272d349125_b";
    NvShader *base = shader_get(key, 0);
    if (!base || base->z_cube_uniform != program->sh[0]->z_cube_uniform) return nil;
    NvShader *shader = zelda_cube_shader(base);
    if (!shader) return nil;
    NvProg *result = [NvProg new]; for (int i = 0; i < 6; i++) result->sh[i] = program->sh[i]; result->sh[0] = shader;
    program->z_cube = result; return result;
}
static NvProg *zelda_cube_prepare(NvProg *program, int primitive) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_CUBE_PASS"); enabled = !value || atoi(value); }   // 0: single-face passes (A/B checks)
    int face = enabled ? zelda_cube_face(&R.rt) : -1;
    // Occlusion counting needs no guard: every face still issues its own draw, reports and resets switch the visibility
    // slot inside a pass (g_vis_cur = -1), and Metal counts samples across all layers, so the reported sums are unchanged.
    // g_vis_on stays 1 after the game's first query, so guarding on it disabled the merge in every real scene.
    if (!z_uniform_gpu || tbdr_on() || primitive != NVN_DRAW_PRIMITIVE_TRIANGLES || S.ds.dt || S.ds.st ||
        z_depth_clear.pending || z_color_clear.count || R.clear_c[0] || R.clear_d || R.clear_s ||
        draw_samples_rt(program)) face = -1;
    NvProg *layered = face >= 0 ? zelda_cube_program(program) : nil;
    if (!layered) face = -1;
    if (g_enc && (z_cube_active != (face >= 0))) rp_flush();
    z_cube_requested = face;
    if (face >= 0) { z_cube_draws++; return layered; }
    return program;
}
