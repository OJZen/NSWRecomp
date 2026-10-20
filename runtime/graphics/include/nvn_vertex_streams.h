#pragma once
// Same NVN attributes as pso_key_fill; absent formats use the separately bound zero buffer.
static unsigned zelda_vertex_streams(const nvn_refl *reflection, const va_st *attributes, int count) {
    if (!reflection || !reflection->route_b || reflection->nv < 0 || reflection->nv > NVN_REFL_MAXV || count < 0 || count > 16)
        return 0xffff;
    unsigned streams = 0;
    for (int i = 0; i < reflection->nv; i++) {
        unsigned a = reflection->v[i].nvn;
        if (a >= 16 || a >= (unsigned)count || !attributes[a].fmt) continue;
        if (attributes[a].stream >= 16) return 0xffff;
        streams |= 1u << attributes[a].stream;
    }
    return streams;
}
