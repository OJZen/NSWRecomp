// Synthetic exclusive-access checks; no game data or platform services.
#include "cpu.h"
#include "simd.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>

extern GuestFn pair_tests[];
void test_clrex(Cpu *c);
static _Thread_local jmp_buf trap;
static _Thread_local int expect_trap;
void rt_udf(Cpu *c, uint64_t pc) {
    (void)c; (void)pc;
    if (expect_trap) longjmp(trap, 1);
    abort();
}

int test_host_rounding(void) {
#if defined(__aarch64__)
    uint64_t saved = rt_get_fpcr(), status = rt_get_fpsr();
    int result = 0;
    const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    for (unsigned controls = 0; controls < 4; controls++) {
        uint64_t control = (saved & ~(15ull << 22)) | ((uint64_t)controls << 24);
        for (unsigned i = 0; i < 4; i++) {
            rt_set_fpcr(control);
            simd_set_host_round(modes[i]);
            if (simd_host_round() != modes[i] || fegetround() != modes[i] ||
                rt_get_fpcr() != (control | ((uint64_t)i << 22))) result = 1;
            simd_set_host_round(modes[i]); // Setting the existing mode preserves every other control and status bit.
            if (rt_get_fpcr() != (control | ((uint64_t)i << 22)) || rt_get_fpsr() != status) result = 2;
        }
    }
    rt_set_fpcr(saved); rt_set_fpsr(status);
    return result;
#else
    return 0;
#endif
}

int test_scalar_exclusives(void) {
    uint64_t cell = 7, other = 11;
    Cpu c = {0};
    uint64_t a = (uintptr_t)&cell, b = (uintptr_t)&other;
    if (stx64(&c, a, 9) != 1 || cell != 7) return 1;
    if (ldx64(&c, a) != 7 || stx64(&c, a, 9) != 0 || cell != 9) return 2;
    if (stx64(&c, a, 12) != 1 || cell != 9) return 3;
    ldx64(&c, a); test_clrex(&c);
    if (stx64(&c, a, 12) != 1 || cell != 9) return 4;
    ldx64(&c, a);
    if (stx64(&c, b, 12) != 1 || stx64(&c, a, 12) != 1 || other != 11) return 5;
    ldx64(&c, a);
    if (stx32(&c, a, 12) != 1 || stx64(&c, a, 12) != 1 || cell != 9) return 6;
    ldx64(&c, a); __atomic_store_n(&cell, 13, __ATOMIC_RELEASE);
    if (stx64(&c, a, 12) != 1 || stx64(&c, a, 12) != 1 || cell != 13) return 7;
    for (unsigned size = 0; size < 4; size++) {
        uint64_t bytes = 0;
        uint64_t p = (uintptr_t)&bytes;
        switch (size) {
        case 0: ldx8(&c, p); if (stx8(&c, p, ~0ull)) return 8; break;
        case 1: ldx16(&c, p); if (stx16(&c, p, ~0ull)) return 9; break;
        case 2: ldx32(&c, p); if (stx32(&c, p, ~0ull)) return 10; break;
        case 3: ldx64(&c, p); if (stx64(&c, p, ~0ull)) return 11; break;
        }
        uint64_t expected = size == 3 ? ~0ull : (1ull << (8 << size)) - 1;
        if (bytes != expected) return 12;
    }
    return 0;
}

int test_misaligned_pair(unsigned index) {
    _Alignas(16) uint64_t cell[4] = {0};
    Cpu c = {0};
    c.x[2] = (uintptr_t)cell + 1;
    expect_trap = 1;
    if (!setjmp(trap)) { pair_tests[index](&c); expect_trap = 0; return 1; }
    expect_trap = 0;
    return 0;
}

typedef struct {
    _Alignas(16) __uint128_t cell;
    uint64_t payload;
    unsigned ack, bad;
    GuestFn load, store;
} Channel;

static void *writer(void *arg) {
    Channel *ch = arg;
    Cpu c = {0};
    c.x[2] = (uintptr_t)&ch->cell;
    for (unsigned i = 1; i <= 20000; i++) {
        while (__atomic_load_n(&ch->ack, __ATOMIC_ACQUIRE) != i - 1) {}
        ch->payload = (uint64_t)i * 17;
        c.x[4] = i; c.x[5] = ~(uint64_t)i;
        ch->store(&c);
    }
    return NULL;
}

static void *reader(void *arg) {
    Channel *ch = arg;
    Cpu c = {0};
    c.x[2] = (uintptr_t)&ch->cell;
    for (unsigned i = 1; i <= 20000; i++) {
        do { ch->load(&c); } while (c.x[0] != i);
        if (c.x[1] != ~(uint64_t)i || ch->payload != (uint64_t)i * 17 || c.x[3]) ch->bad++;
        __atomic_store_n(&ch->ack, i, __ATOMIC_RELEASE);
    }
    return NULL;
}

int test_pair_threads(unsigned load, unsigned store) {
    Channel ch = {.load = pair_tests[load], .store = pair_tests[store]};
    pthread_t w, r;
    if (pthread_create(&w, NULL, writer, &ch)) return -1;
    if (pthread_create(&r, NULL, reader, &ch)) abort();
    pthread_join(w, NULL); pthread_join(r, NULL);
    return ch.bad;
}
