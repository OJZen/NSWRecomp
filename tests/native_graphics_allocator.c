#include "graphics_allocator.h"
#include "dispatch.h"
#include <assert.h>
#include <stdlib.h>

void hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *);
void hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *);
static unsigned calls, nested_calls;
static _Alignas(64) unsigned char storage[256];
static Cpu *original;
static bool unbalanced, fail_allocation;

static void allocation(Cpu *c) {
    assert(c != original && c->sp == original->sp && c->tpidr == original->tpidr);
    assert(c->thread == original->thread && c->fpcr == original->fpcr);
    assert(!c->x[30] && c->excl_addr == UINT64_MAX && !c->excl_size);
    assert(c->x[0] == 128 && c->x[1] == 64 && c->x[2] == 0x123456789abcdef0ull);
    // Re-enter the other allocator while the first callback is running. The
    // registry lock must have been released, and nested Cpu state must be isolated.
    Cpu saved = *c;
    uint64_t result = 0;
    assert(graphics_allocator_call(c, GRAPHICS_DEVTOOLS_ALLOCATOR, GRAPHICS_ALLOCATE, 8, 16, &result));
    assert(result == 0xfedcba9876543210ull && c->fpsr == (saved.fpsr | 4));
    saved.fpsr = c->fpsr;
    assert(!memcmp(c, &saved, sizeof saved));
    c->x[0] = fail_allocation ? 0 : (uintptr_t)storage;
    c->x[19] = 999; c->x[30] = 123; c->nzcv = UINT64_MAX;
    c->v[0] = c->v[31] = (__uint128_t)UINT64_MAX << 64;
    c->fpcr = 0; c->tpidr = 0; c->fpsr |= 1;
    if (unbalanced) c->sp += 16;
    storage[0] = 42;
    calls++;
}
static void devtools(Cpu *c) {
    assert(c->x[0] == 8 && c->x[1] == 16 && c->x[2] == 0x77);
    c->x[0] = 0xfedcba9876543210ull; c->fpsr |= 4; nested_calls++;
    GraphicsAllocatorCallbacks same = {0x2000, 0, 0, 0x77};
    assert(graphics_allocator_set(GRAPHICS_DEVTOOLS_ALLOCATOR, &same));
}
static void release(Cpu *c) {
    assert(c->x[0] == (uintptr_t)storage && c->x[1] == 0x123456789abcdef0ull);
    storage[0] = 0; calls++;
}
static void reallocation(Cpu *c) {
    assert(c->x[0] == (uintptr_t)storage && c->x[1] == 256 && c->x[2] == 0x123456789abcdef0ull);
    c->x[0] = (uintptr_t)storage; calls++;
}
static void eight(Cpu *c) {
    for (unsigned i = 0; i < 8; i++) assert(c->x[i] == i + 1);
    c->x[0] = UINT64_MAX;
}
static void host_token(Cpu *c) { (void)c; abort(); }
GuestFn rt_find_external(uint64_t address) { return address == 0x7000 ? host_token : NULL; }
GuestFn rt_find_function(uint64_t address) {
    switch (address) {
    case 0x1000: return allocation;
    case 0x1004: return release;
    case 0x1008: return reallocation;
    case 0x2000: return devtools;
    case 0x3000: return eight;
    default: return rt_find_external(address);
    }
}
void rt_dispatch(Cpu *c) { GuestFn function = rt_find_function(c->pc); assert(function); function(c); }
void rt_report_missing_host_api(Cpu *c, const char *name) {
    (void)c; assert(!strcmp(name, "invalid graphics allocator callback"));
}

int main(int argc, char **argv) {
    (void)argv;
    Cpu c = {0}; c.sp = 0x100000; c.tpidr = 0x200000; c.tpidrro = 0x210000;
    c.x[19] = 19; c.excl_addr = 16; c.excl_size = 64; c.fpcr = 0x400000;
    c.thread = &c; original = &c;
    uint64_t result = 99;
    assert(!graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_ALLOCATE, 128, 64, &result));
    assert(result == 99);
    c.x[0] = 0x2000; c.x[1] = c.x[2] = 0; c.x[3] = 0x77;
    hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(&c);
    c.x[0] = argc > 1 ? 0x7000 : 0x1000; c.x[1] = 0x1004; c.x[2] = 0x1008;
    c.x[3] = 0x123456789abcdef0ull;
    Cpu saved = c;
    hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(&c);
    assert(!memcmp(&c, &saved, sizeof c));
    assert(graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_ALLOCATE, 128, 64, &result));
    assert(result == (uintptr_t)storage && storage[0] == 42 && c.fpsr == 5);
    saved.fpsr = 5; assert(!memcmp(&c, &saved, sizeof c));
    assert(graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_REALLOCATE, result, 256, &result));
    assert(result == (uintptr_t)storage);
    assert(graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_FREE, result, 0, NULL));
    assert(!storage[0] && calls == 3 && nested_calls == 1);
    // Invalid replacements must not partially overwrite the prior registration.
    GraphicsAllocatorCallbacks invalid = {0x2000, 0x9998, 0, 0x77};
    assert(!graphics_allocator_set(GRAPHICS_ALLOCATOR, &invalid));
    fail_allocation = true;
    assert(graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_ALLOCATE, 128, 64, &result));
    assert(!result); // A guest allocation failure is a valid callback result.
    unbalanced = true; result = 99;
    assert(!graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, GRAPHICS_ALLOCATE, 128, 64, &result));
    assert(result == 99 && !memcmp(&c, &saved, sizeof c));
    assert(!graphics_allocator_call(&c, GRAPHICS_DEVTOOLS_ALLOCATOR, GRAPHICS_FREE, 0, 0, NULL));
    assert(!graphics_allocator_call(&c, (GraphicsAllocatorKind)-1, GRAPHICS_ALLOCATE, 0, 0, NULL));
    assert(!graphics_allocator_call(&c, GRAPHICS_ALLOCATOR, (GraphicsAllocatorOperation)99, 0, 0, NULL));
    const uint64_t args[] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(guest_callback(&c, 0x3000, args, 8, &result) && result == UINT64_MAX);
    assert(!guest_callback(&c, 0x3000, args, 9, &result));
    assert(!guest_callback(&c, 0x3000, NULL, 1, &result));
    assert(!guest_callback(NULL, 0x3000, args, 8, &result));
    for (unsigned i = 0; i < 4; i++) {
        uint64_t address = (uint64_t[]){0, 0x3001, 0x9998, 0x7000}[i];
        assert(!guest_callback(&c, address, args, 8, &result));
    }
    c.sp += 8; assert(!guest_callback(&c, 0x3000, args, 8, &result));
    return 0;
}
