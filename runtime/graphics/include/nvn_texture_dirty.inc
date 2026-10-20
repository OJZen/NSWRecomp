#pragma once
// Dirty tracking for packaged textures. A texture in an aliased virtual pool (nvn_virtual.inc) shares pages with its
// physical pool, and the game may write through either mapping: both ranges are tracked and the texture counts as
// changed when either is. Without this, every bind of such a texture was a full re-upload (v1.4.3 post-cutscene
// scene: about 7-8 MB per frame, plus a new texture, a staging buffer and a residency commit each time).
uint64_t nvn_canonical_address(uint64_t address, uint64_t length);
static const uint8_t *zelda_texture_alias_of(const tex_t *t, int *bad) {
    uint64_t physical = nvn_canonical_address((uintptr_t)t->mem, t->L.size);
    *bad = !physical;  // straddles an alias boundary: not trackable
    return physical && physical != (uintptr_t)t->mem ? (const uint8_t *)(uintptr_t)physical : NULL;
}
// The alias of a texture never changes: virtual pools are mapped once and remapping is refused.
static const uint8_t *zelda_texture_alias(NvPkg *pk, const tex_t *t, int *bad) {
    if (!pk->z_alias_known) { int b; pk->z_alias = zelda_texture_alias_of(t, &b); pk->z_alias_bad = (uint8_t)b; pk->z_alias_known = 1; }
    *bad = pk->z_alias_bad;
    return pk->z_alias;
}
static int zelda_texture_tracked(NvPkg *pk, const tex_t *t) {
    int bad; const uint8_t *alias = zelda_texture_alias(pk, t, &bad);
    return !bad && nvn_dirty_tracked(t->mem, t->L.size) && (!alias || nvn_dirty_tracked(alias, t->L.size));
}
static int zelda_texture_dirty_since(NvPkg *pk, const tex_t *t, uint64_t stamp) {
    int bad; const uint8_t *alias = zelda_texture_alias(pk, t, &bad);
    return nvn_dirty_since(t->mem, t->L.size, stamp) || (alias && nvn_dirty_since(alias, t->L.size, stamp));
}
static void zelda_texture_protect(const tex_t *t) {
    int bad; const uint8_t *alias = zelda_texture_alias_of(t, &bad);
    nvn_dirty_protect(t->mem, t->L.size);
    if (alias) nvn_dirty_protect(alias, t->L.size);
}
