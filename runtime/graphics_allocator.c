#include "graphics_allocator.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static GraphicsAllocatorCallbacks allocators[2];

bool graphics_allocator_set(GraphicsAllocatorKind kind, const GraphicsAllocatorCallbacks *callbacks) {
    if ((unsigned)kind >= 2 || !callbacks ||
        (callbacks->allocate && !guest_callback_address(callbacks->allocate)) ||
        (callbacks->free && !guest_callback_address(callbacks->free)) ||
        (callbacks->reallocate && !guest_callback_address(callbacks->reallocate))) return false;
    pthread_mutex_lock(&lock);
    allocators[kind] = *callbacks;
    pthread_mutex_unlock(&lock);
    return true;
}

bool graphics_allocator_call(Cpu *caller, GraphicsAllocatorKind kind, GraphicsAllocatorOperation operation,
                             uint64_t first, uint64_t second, uint64_t *result) {
    if ((unsigned)kind >= 2) return false;
    pthread_mutex_lock(&lock);
    GraphicsAllocatorCallbacks callbacks = allocators[kind];
    pthread_mutex_unlock(&lock); // Guest re-entry may register or invoke another allocator.
    uint64_t address, arguments[3] = {first, second, callbacks.user};
    size_t count = 3;
    switch (operation) {
    case GRAPHICS_ALLOCATE: address = callbacks.allocate; break;
    case GRAPHICS_FREE: address = callbacks.free; arguments[1] = callbacks.user; count = 2; break;
    case GRAPHICS_REALLOCATE: address = callbacks.reallocate; break;
    default: return false;
    }
    return guest_callback(caller, address, arguments, count, result);
}

extern void rt_report_missing_host_api(Cpu *, const char *);
static void register_allocator(Cpu *c, GraphicsAllocatorKind kind) {
    GraphicsAllocatorCallbacks callbacks = {c->x[0], c->x[1], c->x[2], c->x[3]};
    if (!graphics_allocator_set(kind, &callbacks)) {
        rt_report_missing_host_api(c, "invalid graphics allocator callback");
        fputs("invalid compiled graphics allocator callback\n", stderr);
        exit(78);
    }
    fprintf(stderr, "GRAPHICS allocator registered: %s\n", kind == GRAPHICS_ALLOCATOR ? "graphics" : "devtools");
}

void hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *c) {
    register_allocator(c, GRAPHICS_ALLOCATOR);
}
void hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *c) {
    register_allocator(c, GRAPHICS_DEVTOOLS_ALLOCATOR);
}
