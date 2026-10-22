#include "dispatch.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

static void add_one(Cpu *c) { c->x[0] += 1; }
static void add_ten(Cpu *c) { c->x[0] += 10; }
static void add_hundred(Cpu *c) { c->x[0] += 100; }
static void add_thousand(Cpu *c) { c->x[0] += 1000; }
const uint64_t rtld_fn_addrs[] = {0x100000, 0x100004, 0x104000, 0x104004};
const GuestFn rtld_fn_ptrs[] = {add_one, add_ten, add_hundred, add_thousand};
const uint64_t rtld_fn_count = 4;
static int external_version;
GuestFn rt_find_external(uint64_t address) {
    if (address != 0x800000) return NULL;
    return external_version ? add_thousand : add_one;
}
static void *worker(void *argument) {
    Cpu c = {0}; unsigned seed = (unsigned)(uintptr_t)argument;
    const unsigned amounts[] = {1, 10, 100, 1000};
    for (unsigned i = 0; i < 10000; i++) {
        unsigned index = (i + seed) % 4;
        c.pc = rtld_fn_addrs[index]; c.x[0] = 0;
        rt_dispatch(&c); assert(c.x[0] == amounts[index]);
        rt_dispatch(&c); assert(c.x[0] == 2 * amounts[index]);
    }
    return NULL;
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { Cpu missing = {0}; rt_dispatch(&missing); return 1; }
    assert(!rt_validate_tables());
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    // Two pairs collide in the direct-mapped cache; external bindings must always be re-resolved.
    Cpu c = {.pc = 0x800000};
    rt_dispatch(&c); assert(c.x[0] == 1);
    external_version = 1; rt_dispatch(&c); assert(c.x[0] == 1001);
    assert(!rt_find_function(0) && !rt_find_function(0x100001) && !rt_find_function(UINT64_MAX));
    return 0;
}
