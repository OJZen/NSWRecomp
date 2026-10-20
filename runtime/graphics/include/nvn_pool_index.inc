// Address -> memory pool lookup. v1.4.3 keeps about 2,900 pools alive in the post-cutscene scene and every draw
// resolves several addresses, so the original scan of every pool per lookup cost milliseconds per frame. A sorted
// index is rebuilt after pools are added or removed; ranges are not expected to nest, and a miss still falls back
// to the original scan so the result is never narrower than before. The caller holds g_mu.
static int *z_pool_order, z_pool_order_cap, z_pool_sorted_dirty = 1;
static int z_pool_hint[64];
static int zelda_pool_compare(const void *a, const void *b) {
    uint64_t x = g_pools[*(const int *)a].lo, y = g_pools[*(const int *)b].lo;
    return (x > y) - (x < y);
}
static int zelda_pool_find(uint64_t addr, uint64_t len) {
    if (len > UINT64_MAX - addr) return -1;
    static int cached = -1;
    if (cached < 0) { const char *e = getenv("ZELDA_POOL_LOOKUP_CACHE"); cached = !e || atoi(e); }
    if (z_pool_sorted_dirty) {
        if (z_pool_order_cap < g_npool) {
            z_pool_order_cap = g_cap; z_pool_order = realloc(z_pool_order, (size_t)z_pool_order_cap * sizeof *z_pool_order);
            if (!z_pool_order) MDIE("Memory pool index allocation failed");
        }
        for (int i = 0; i < g_npool; i++) z_pool_order[i] = i;
        qsort(z_pool_order, (size_t)g_npool, sizeof *z_pool_order, zelda_pool_compare);
        for (unsigned i = 0; i < 64; i++) z_pool_hint[i] = -1;
        z_pool_sorted_dirty = 0;
    }
    unsigned hint = ((addr >> 12) ^ (addr >> 21)) & 63;
    int found = cached ? z_pool_hint[hint] : -1;
    if (found >= 0) {
        int i = z_pool_order[found];
        // A nested pool may begin inside this range; preserve the sorted lookup's preferred owner.
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi &&
            (found + 1 == g_npool || addr < g_pools[z_pool_order[found + 1]].lo)) return i;
    }
    int lo = 0, hi = g_npool - 1, last = -1;
    while (lo <= hi) { int mid = lo + (hi - lo) / 2; if (g_pools[z_pool_order[mid]].lo <= addr) { last = mid; lo = mid + 1; } else hi = mid - 1; }
    if (last >= 0) { int i = z_pool_order[last]; if (addr + len <= g_pools[i].hi) { if (cached) z_pool_hint[hint] = last; return i; } }
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return i;
    return -1;
}
