// Write-protect dirty-page tracking for guest memory pools; same API and page protocol as D2R dm/gfx/nvn/nvn_dirty.c.
// D2R keeps one page table per pool and resolves an address to the first pool that covers it. Zelda places small pools
// 4 KiB apart on its heap, so with 16 KiB host pages neighbouring pools share a boundary page: textures in the second
// pool were reported untracked and re-uploaded on every bind (v1.4.3 post-cutscene scene: 7-8 MB per frame), and a
// write fault was recorded only in the first pool's copy of the shared page. Here page state lives once per host page
// (two-level table keyed by address) with a count of the pools covering it.
// Signal handler constraint: the fault path only reads the published tables and uses atomics and mprotect.
#include "nvn_dirty.h"
#include "host_compat.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>

// Page states (see D2R for the ordering argument): set read-only NONE -> BUSY -> mprotect -> PROT;
// unprotect PROT -> BUSY -> mprotect -> NONE. A NONE page is always writable and a PROT page always read-only.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
enum { CHUNK_SHIFT = 12, CHUNK_PAGES = 1 << CHUNK_SHIFT };  // 4096 pages = 64 MiB of address space per chunk
enum { VA_BITS = 47 };
typedef struct { _Atomic uint8_t state[CHUNK_PAGES]; _Atomic uint16_t refs[CHUNK_PAGES]; _Atomic uint64_t gen[CHUNK_PAGES]; } chunk_t;
static chunk_t *_Atomic *g_top;   // reserved lazily (untouched entries cost no memory); chunks are never freed
static size_t g_top_n;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps, g_adds_total;
static _Atomic int g_live, g_live_peak;
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;
static mach_timebase_info_data_t g_tb;
static _Atomic int g_busy_red;
// Registered pools (creation / destruction only; never read on the fault path).
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uintptr_t mem; size_t size; } *g_pools;
static int g_npools, g_cap_pools, g_used_pools;

static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}
// Page slot of host page number `page`, or NULL when no pool covers it.
static chunk_t *page_chunk(uintptr_t page, unsigned *index) {
    uintptr_t top = page >> CHUNK_SHIFT;
    if (!g_top || top >= g_top_n) return NULL;
    chunk_t *c = atomic_load_explicit(&g_top[top], memory_order_acquire);
    *index = (unsigned)(page & (CHUNK_PAGES - 1));
    return c && atomic_load_explicit(&c->refs[*index], memory_order_acquire) ? c : NULL;
}
static int busy_wait(chunk_t *c, unsigned i, uintptr_t page) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&c->state[i]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("ERROR: Dirty tracking: page %p stayed busy for more than %d ms; protection update did not complete", (void *)(page << g_shift), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static void unprotect_page(chunk_t *c, unsigned i, uintptr_t page) {  // caller moved the state PROT -> BUSY
    mp(page << g_shift, g_pg, PROT_READ | PROT_WRITE);
    atomic_store(&c->gen[i], atomic_fetch_add(&g_now, 1) + 1);
    atomic_store(&c->state[i], PG_NONE);
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t page = (uintptr_t)addr >> g_shift; unsigned i;
    chunk_t *c = page_chunk(page, &i);
    if (!c) return 0;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&c->state[i], &st, PG_BUSY)) { unprotect_page(c, i, page); atomic_fetch_add(&g_faults, 1); return 1; }
    if (st == PG_BUSY) return busy_wait(c, i, page);
    return atomic_load(&c->gen[i]) != 0;  // unprotected by another thread just before this fault: re-execute
}
static void unprotect_range(uintptr_t a, uintptr_t e, int count) {
    for (uintptr_t page = a >> g_shift, last = (e - 1) >> g_shift; a < e && page <= last; page++) {
        unsigned i; chunk_t *c = page_chunk(page, &i);
        if (!c) continue;
        for (;;) { uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&c->state[i], &st, PG_BUSY)) { unprotect_page(c, i, page); if (count) atomic_fetch_add(&g_preps, 1); break; }
            if (st != PG_BUSY || !busy_wait(c, i, page)) break; }
    }
}
static void prep_write(void *addr, size_t len) { if (len) unprotect_range((uintptr_t)addr, (uintptr_t)addr + len, 1); }

void nvn_dirty_add_range(const void *mem, size_t size) {
    pthread_mutex_lock(&g_mu);
    if (!g_top) {
        g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb);
        g_top_n = ((size_t)1 << (VA_BITS - g_shift)) >> CHUNK_SHIFT;
        void *top = mmap(NULL, g_top_n * sizeof *g_top, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (top == MAP_FAILED) die("Failed to reserve dirty-page table");
        g_top = top;
        dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write);
    }
    atomic_fetch_add(&g_adds_total, 1);
    if (!size) { pthread_mutex_unlock(&g_mu); return; }
    if (g_npools == g_cap_pools) {
        g_cap_pools = g_cap_pools ? g_cap_pools * 2 : 1024;
        g_pools = realloc(g_pools, (size_t)g_cap_pools * sizeof *g_pools);
        if (!g_pools) die("Failed to allocate dirty-pool table");
    }
    g_pools[g_npools].mem = (uintptr_t)mem; g_pools[g_npools].size = size;
    g_npools++; if (g_npools > g_used_pools) g_used_pools = g_npools;
    for (uintptr_t page = (uintptr_t)mem >> g_shift, last = ((uintptr_t)mem + size - 1) >> g_shift; page <= last; page++) {
        uintptr_t top = page >> CHUNK_SHIFT;
        if (top >= g_top_n) die("Dirty-tracking address out of range: %p", mem);
        chunk_t *c = atomic_load_explicit(&g_top[top], memory_order_relaxed);
        if (!c) {
            if (!(c = calloc(1, sizeof *c))) die("Failed to allocate dirty-page table");
            atomic_store_explicit(&g_top[top], c, memory_order_release);
        }
        unsigned i = (unsigned)(page & (CHUNK_PAGES - 1));
        if (atomic_load(&c->refs[i]) == UINT16_MAX) die("Dirty-page reference count overflow: %p", (void *)(page << g_shift));
        atomic_fetch_add_explicit(&c->refs[i], 1, memory_order_release);
    }
    int live = atomic_fetch_add(&g_live, 1) + 1, peak = atomic_load(&g_live_peak);
    while (live > peak && !atomic_compare_exchange_weak(&g_live_peak, &peak, live)) {}
    pthread_mutex_unlock(&g_mu);
}
// Pool destruction: pages no other pool covers are made writable again (the memory returns to the game) before
// they stop being tracked; shared boundary pages stay with the neighbouring pool.
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_mu);
    int k = 0;
    while (k < g_npools && g_pools[k].mem != (uintptr_t)mem) k++;
    if (k < g_npools) {
        uintptr_t a = g_pools[k].mem; size_t size = g_pools[k].size;
        g_pools[k] = g_pools[--g_npools];
        for (uintptr_t page = a >> g_shift, last = (a + size - 1) >> g_shift; page <= last; page++) {
            chunk_t *c = atomic_load(&g_top[page >> CHUNK_SHIFT]); unsigned i = (unsigned)(page & (CHUNK_PAGES - 1));
            if (atomic_load(&c->refs[i]) == 1) unprotect_range(page << g_shift, (page + 1) << g_shift, 0);
            atomic_fetch_sub_explicit(&c->refs[i], 1, memory_order_release);
        }
        atomic_fetch_sub(&g_live, 1);
    }
    pthread_mutex_unlock(&g_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    if (!len) len = 1;
    for (uintptr_t page = (uintptr_t)addr >> g_shift, last = ((uintptr_t)addr + len - 1) >> g_shift; page <= last; page++) {
        unsigned i; if (!page_chunk(page, &i)) return 0;
    }
    return g_top != NULL;
}
// Consecutive pages taken NONE -> BUSY are protected with one mprotect; PROT / BUSY / untracked pages break a run.
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    uintptr_t first = (uintptr_t)addr >> g_shift, last = ((uintptr_t)addr + len - 1) >> g_shift, run = 0;
    int open = 0;
    for (uintptr_t page = first; page <= last + 1; page++) {
        unsigned i = 0; chunk_t *c = page <= last ? page_chunk(page, &i) : NULL;
        uint8_t st = PG_NONE;
        int took = c && atomic_compare_exchange_strong(&c->state[i], &st, PG_BUSY);
        if (took && !open) { run = page; open = 1; }
        if (!took && open) {
            mp(run << g_shift, (page - run) << g_shift, PROT_READ);
            // State arrays are never freed, so a pool destroyed meanwhile still finds its page slot here.
            for (uintptr_t k = run; k < page; k++) atomic_store(&atomic_load(&g_top[k >> CHUNK_SHIFT])->state[k & (CHUNK_PAGES - 1)], PG_PROT);
            open = 0;
        }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;  // untracked: treat as dirty rather than show stale content
    for (uintptr_t page = (uintptr_t)addr >> g_shift, last = ((uintptr_t)addr + len - 1) >> g_shift; page <= last; page++) {
        unsigned i; chunk_t *c = page_chunk(page, &i);
        if (!c || atomic_load(&c->gen[i]) > stamp) return 1;
    }
    return 0;
}
void nvn_dirty_stat(int *live, int *peak, int *cap) {
    *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak);
    pthread_mutex_lock(&g_mu); *cap = g_cap_pools; pthread_mutex_unlock(&g_mu);
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    *live = atomic_load(&g_live);
    pthread_mutex_lock(&g_mu); *used_slots = g_used_pools; pthread_mutex_unlock(&g_mu);
    *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
