#include "dispatch.h"
#include "kernel.h"
#include "threads.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    atomic_uint mutex, key, address, ready, returned;
    uint32_t child, event, mode, core;
    uint64_t stack, tls;
} Shared;
static Shared *shared;
static uint64_t entry;
static _Thread_local bool child;
void (*threads_test_before_wait)(void);
void (*threads_test_main_quiescent)(void);
static atomic_bool main_parked;
static void parked(void) { atomic_store(&main_parked, true); }

static void ready(void) { if (child) atomic_store(&shared->ready, 1); }
static void *external_stop(void *unused) {
    (void)unused;
    assert(threads_stop()); // Must wait for the guest main thread as well as child workers.
    assert(atomic_load(&main_parked));
    _Exit(0);
}
static void svc(unsigned number, uint64_t *r) {
    threads_checkpoint();
    assert(kernel_svc(number, r));
    threads_checkpoint();
}

static void guest(Cpu *cpu) {
    child = true;
    assert(cpu->x[0] == (uintptr_t)shared && cpu->sp == shared->stack + 0x10000);
    assert((uintptr_t)&cpu < shared->stack || (uintptr_t)&cpu >= cpu->sp);
    assert(cpu->tpidrro && cpu->tpidr && !LD64(cpu->tpidr) && !cpu->fpcr && !cpu->fpsr);
    assert(LD64(cpu->tpidrro + 0x1f8) && cpu->tpidrro != shared->tls);
    shared->tls = cpu->tpidrro;
    uint64_t r[32] = {0, 0xffff8000u};
    svc(0x25, r); assert(!r[0] && r[1]);
    svc(0x10, r); assert(r[0] == shared->core);
    if (!shared->mode) {
        r[0] = (uintptr_t)&shared->mutex; r[1] = (uintptr_t)&shared->key; r[2] = shared->child; r[3] = 2000000000;
        svc(0x1c, r); assert(!r[0] && atomic_load(&shared->mutex) == shared->child);
        r[0] = (uintptr_t)&shared->mutex; svc(0x1b, r); assert(!r[0]);
        r[1] = (uintptr_t)&shared->event; r[2] = 1; r[3] = 0;
        svc(0x18, r); assert(r[0] == 0xec01); // Cancellation survived the condition/mutex wait.
    } else if (shared->mode <= 2) {
        r[1] = (uintptr_t)&shared->event; r[2] = 1; r[3] = UINT64_MAX;
        svc(0x18, r); assert(r[0] == (shared->mode == 1 ? 0xec01u : 0));
    } else {
        r[0] = (uintptr_t)&shared->address; r[1] = 2; r[2] = INT_MAX; r[3] = UINT64_MAX;
        svc(0x34, r);
        assert(shared->mode == 3 && !r[0] && atomic_load(&shared->address) == 0x80000000u);
    }
    atomic_store(&shared->returned, 1);
    svc(0x0a, r);
    abort();
}

GuestFn rt_find_function(uint64_t address) { return address == entry ? guest : NULL; }
void rt_dispatch(Cpu *cpu) { GuestFn fn = rt_find_function(cpu->pc); assert(fn); fn(cpu); }

static uint64_t info(unsigned type) {
    uint64_t r[32] = {0, type, 0xffff8001u}; svc(0x29, r); assert(!r[0]); return r[1];
}

int main(int argc, char **argv) {
    (void)argv;
    alarm(20);
    unsigned char *memory = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    LoadedImage image = {.count = 1, .bytes = 0x10000};
    image.modules[0].header = (LoadHeader){.base = (uintptr_t)memory, .size = 0x10000,
        .text_size = 0x1000, .data_offset = 0x8000, .data_size = 0x8000};
    uint32_t main_handle;
    int initialized = kernel_init(&image, (uintptr_t)memory + 0x10000, 0x10000, &main_handle);
    if (initialized) { munmap(memory, 0x20000); return initialized; }
    if (argc > 1) {
        threads_test_main_quiescent = parked;
        pthread_t host; assert(!pthread_create(&host, NULL, external_stop, NULL));
        for (;;) { threads_checkpoint(); sched_yield(); }
    }
    shared = (void *)(memory + 0x8000); entry = (uintptr_t)memory + 0x80;
    threads_test_before_wait = ready;
    uint64_t alias = info(2), stack = info(14), r[32] = {alias, 0x10000};
    svc(0x2c, r); assert(!r[0]);
    for (unsigned iteration = 0; iteration < 33; iteration++) {
        shared->mode = iteration == 32 ? 4 : iteration % 4;
        shared->core = iteration % 3;
        shared->stack = stack;
        atomic_store(&shared->ready, 0); atomic_store(&shared->returned, 0);
        atomic_store(&shared->address, INT_MAX); atomic_store(&shared->key, 0);
        r[0] = stack; r[1] = alias; r[2] = 0x10000; svc(4, r); assert(!r[0]);
        uint32_t writer, reader; assert(kernel_event_create(&writer, &reader)); shared->event = reader;
        r[1] = entry; r[2] = (uintptr_t)shared; r[3] = stack + 0x10000; r[4] = 44; r[5] = shared->core;
        svc(8, r); assert(!r[0]); shared->child = (uint32_t)r[1];
        atomic_store(&shared->mutex, shared->child);
        if (shared->mode == 1) { r[0] = shared->child; svc(0x19, r); assert(!r[0]); }
        r[0] = shared->child; svc(9, r); assert(!r[0]);
        r[0] = shared->child; svc(9, r); assert(r[0] == 0xfa01);
        while (!atomic_load(&shared->ready)) sched_yield();
        if (!shared->mode) {
            uint32_t expected = 0;
            while (!atomic_compare_exchange_weak(&shared->mutex, &expected, main_handle)) { expected = 0; sched_yield(); }
            r[0] = shared->child; svc(0x19, r); assert(!r[0]);
            r[0] = (uintptr_t)&shared->key; r[1] = 1; svc(0x1d, r); assert(!r[0]);
            r[0] = (uintptr_t)&shared->mutex; svc(0x1b, r); assert(!r[0]);
        } else if (shared->mode == 2) {
            // The waiter holds an object reference even after its public read handle closes.
            r[0] = reader; svc(0x16, r); assert(!r[0]);
            r[0] = writer; svc(0x11, r); assert(!r[0]);
        } else if (shared->mode == 3) {
            r[0] = (uintptr_t)&shared->address; r[1] = 1; r[2] = INT_MAX; r[3] = 1;
            svc(0x35, r); assert(!r[0]);
        } else if (shared->mode == 4) {
            assert(threads_stop() && !atomic_load(&shared->returned));
            break;
        }
        r[1] = (uintptr_t)&shared->child; r[2] = 1; r[3] = 2000000000;
        svc(0x18, r); assert(!r[0] && !r[1] && atomic_load(&shared->returned));
        r[0] = stack; r[1] = alias; r[2] = 0x10000; svc(5, r); assert(!r[0]);
        r[0] = shared->child; svc(0x16, r); assert(!r[0]);
        if (shared->mode != 2) { r[0] = reader; svc(0x16, r); assert(!r[0]); }
        r[0] = writer; svc(0x16, r); assert(!r[0]);
    }
    kernel_destroy();
    munmap(memory, 0x20000);
    return 0;
}
