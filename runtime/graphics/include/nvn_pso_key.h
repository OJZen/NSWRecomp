// Only prewarm alternative attachment layouts; runtime keys and render targets remain unchanged.
static unsigned zelda_pso_optional_colors(const pso_key *key, unsigned fetched) {
    unsigned mask = 0;
    for (unsigned i = 0; i < 8; i++)
        if (key->cfmt[i] && !(key->chmask >> (4 * i) & 15) && !(fetched >> i & 1)) mask |= 1u << i;
    // shortcut: at most three derived pipelines per record; expand only with measured startup/memory headroom.
    return __builtin_popcount(mask) <= 2 ? mask : 0;
}
// Match only state read by pso_desc; disabled blending and unattached colour slots do not affect a pipeline.
static void zelda_pso_normalize(pso_key *key, int merged) {
    if (merged) return; // Merged passes remap logical slots; keep their existing keys.
    for (unsigned i = 0; i < 8; i++) {
        if (!key->cfmt[i]) {
            key->blend_en &= ~(1u << i);
            key->chmask &= ~(15u << (4 * i));
        }
        blend_st value = key->blend[i];
        memset(&key->blend[i], 0, sizeof key->blend[i]);
        if (key->blend_en >> i & 1) {
            key->blend[i].src_rgb = value.src_rgb; key->blend[i].dst_rgb = value.dst_rgb;
            key->blend[i].src_a = value.src_a; key->blend[i].dst_a = value.dst_a;
            key->blend[i].eq_rgb = value.eq_rgb; key->blend[i].eq_a = value.eq_a;
        }
    }
}
