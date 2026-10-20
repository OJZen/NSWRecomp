static int z_cube_requested = -1, z_cube_active;
static uint64_t z_cube_passes, z_cube_draws;
static int zelda_cube_face(const p_set_rt *rt) {
    if (rt->n != 1 || !rt->color[0] || rt->depth || rt->color[0]->d.target != NVN_TEXTURE_TARGET_CUBEMAP ||
        !rt->has_cview[0]) return -1;
    const view_t *v = &rt->cview[0];
    if (!v->set_layers || v->nlayers != 1 || v->min_layer >= 6 ||
        (v->set_format && v->format != rt->color[0]->d.format)) return -1;
    return v->min_layer;
}
static int zelda_cube_same(const p_set_rt *a, const p_set_rt *b) {
    if (!z_cube_active || zelda_cube_face(a) < 0 || zelda_cube_face(b) < 0 || a->color[0]->serial != b->color[0]->serial) return 0;
    view_t av = a->cview[0], bv = b->cview[0]; av.min_layer = bv.min_layer = 0;
    return !memcmp(&av, &bv, sizeof av);
}
static void zelda_cube_descriptor(MTL4RenderPassDescriptor *pass, int drawing) {
    z_cube_active = drawing && z_cube_requested >= 0 && pass.colorAttachments[0].texture.textureType == MTLTextureTypeCube &&
        pass.colorAttachments[0].texture.sampleCount == 1 && pass.colorAttachments[0].loadAction == MTLLoadActionLoad;
    if (z_cube_active) { pass.colorAttachments[0].slice = 0; pass.renderTargetArrayLength = 6; z_cube_passes++; }
}
#include "nvn_cube_source.h"
