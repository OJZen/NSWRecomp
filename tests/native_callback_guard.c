// Synthetic completion callback with a deliberately late waiter.
#include "callback_guard.h"
#include "dispatch.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef struct { uint64_t prefix[2]; pthread_mutex_t mutex; pthread_cond_t condition; uint64_t result; char padding[64]; } Object;
typedef struct { uint64_t tag; Object *object; } Closure;
extern Object state;
extern Closure closure;
extern atomic_bool queued, attempted, waiting, returned;
_Static_assert(offsetof(Object, mutex) == 16 && offsetof(Closure, object) == 8, "synthetic layout");

#ifdef GUARD_ORIGINAL
void f_7200000080(Cpu *c) {
    assert(c->x[0] == (uintptr_t)&closure && c->x[19] == 0x1234 && c->v[7] == 0x9876);
    // Without the wrapper, the dispatcher reaches this before the waiter is
    // permitted to wait. Guarding with the same mutex establishes the ordering.
    assert(atomic_load(&waiting) && !atomic_load(&returned));
    state.result = 42;
    assert(!pthread_cond_signal(&state.condition));
    assert(!atomic_load(&returned)); // Still protected until the wrapper unlocks.
    c->x[0] = 123; c->v[7] = 0x4567; c->fpsr = 0x10;
}
#else
Object state = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
Closure closure = {.object = &state};
atomic_bool queued, attempted, waiting, returned;
static bool missing_helper;
void f_7200000080(Cpu *c);
static GuestFn table[] = {f_7200000080};

static void clobber(Cpu *c) { c->x[0] = 0; c->x[19] = 0; c->x[30] = 0; c->v[7] = 0; }
static void lock_guest(Cpu *c) {
    assert(c->x[0] == (uintptr_t)&state.mutex && c->tpidr == 0x12340000 && c->tpidrro == 0x12341000);
    atomic_store(&attempted, true);
    assert(!pthread_mutex_lock(&state.mutex)); clobber(c);
}
static void unlock_guest(Cpu *c) {
    assert(c->x[0] == (uintptr_t)&state.mutex && c->tpidr == 0x12340000 && c->tpidrro == 0x12341000);
    assert(!pthread_mutex_unlock(&state.mutex)); clobber(c);
}
GuestFn rt_find_external(uint64_t address) { (void)address; return NULL; }
GuestFn rt_find_function(uint64_t address) {
    if (missing_helper) return NULL;
    return address == 0x7200010090ull ? lock_guest : address == 0x72000100a0ull ? unlock_guest : NULL;
}
void rt_dispatch(Cpu *c) { GuestFn f = rt_find_function(c->pc); assert(f); f(c); }
bool kernel_access(uint64_t address, size_t bytes, unsigned permissions) {
    (void)permissions;
    return (address >= (uintptr_t)&closure && address - (uintptr_t)&closure <= sizeof closure && bytes <= sizeof closure - (address - (uintptr_t)&closure)) ||
           (address >= (uintptr_t)&state && address - (uintptr_t)&state <= sizeof state && bytes <= sizeof state - (address - (uintptr_t)&state));
}
_Noreturn void rt_report_adapter_error(Cpu *c, const char *message) { (void)c; fprintf(stderr, "adapter_guard_error: %s\n", message); _Exit(78); }
static void *waiter(void *unused) {
    (void)unused;
    assert(!pthread_mutex_lock(&state.mutex)); atomic_store(&queued, true);
    while (!atomic_load(&attempted)) sched_yield();
    atomic_store(&waiting, true);
    while (state.result != 42) assert(!pthread_cond_wait(&state.condition, &state.mutex));
    atomic_store(&returned, true);
    assert(!pthread_mutex_unlock(&state.mutex)); return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    Cpu cpu = {.sp = 0x10000, .tpidr = 0x12340000, .tpidrro = 0x12341000};
    cpu.x[0] = (uintptr_t)&closure; cpu.x[19] = 0x1234; cpu.x[30] = 0x8888; cpu.v[7] = 0x9876;
    if (!strcmp(argv[1], "bad-context")) cpu.x[0] = UINT64_MAX;
    if (!strcmp(argv[1], "bad-mutex")) closure.object = (void *)(UINTPTR_MAX - 8);
    if (!strcmp(argv[1], "missing-helper")) missing_helper = true;
    pthread_t thread; assert(!pthread_create(&thread, NULL, waiter, NULL));
    while (!atomic_load(&queued)) sched_yield();
    if (!strcmp(argv[1], "direct")) f_7200000080(&cpu);
    else table[0](&cpu);
    assert(!pthread_join(thread, NULL) && atomic_load(&returned));
    assert(callback_guards_completed() == 1 && strlen(callback_guards_sha256) == 64);
    assert(cpu.x[0] == 123 && cpu.x[19] == 0x1234 && cpu.x[30] == 0x8888 && cpu.v[7] == 0x4567 && cpu.fpsr == 0x10);
    assert(cpu.sp == 0x10000 && cpu.tpidr == 0x12340000 && cpu.tpidrro == 0x12341000);
    assert(!pthread_mutex_destroy(&state.mutex) && !pthread_cond_destroy(&state.condition));
    puts("Delayed waiter, completion ordering and guest register/TLS preservation passed");
    return 0;
}
#endif
