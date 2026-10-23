#pragma once
#include "guest_callback.h"

typedef enum { GRAPHICS_ALLOCATOR, GRAPHICS_DEVTOOLS_ALLOCATOR } GraphicsAllocatorKind;
typedef enum { GRAPHICS_ALLOCATE, GRAPHICS_FREE, GRAPHICS_REALLOCATE } GraphicsAllocatorOperation;
typedef struct {
    uint64_t allocate, free, reallocate, user;
} GraphicsAllocatorCallbacks;

// Null entries are retained, but cannot be invoked. Replacement is atomic with
// respect to snapshots; callers must keep registered guest code and user data alive.
bool graphics_allocator_set(GraphicsAllocatorKind kind, const GraphicsAllocatorCallbacks *callbacks);
// allocate(size, alignment, user), free(pointer, user), reallocate(pointer, size, user).
// Requires an active guest Cpu on the calling thread; no host pointer cast/call.
bool graphics_allocator_call(Cpu *caller, GraphicsAllocatorKind kind, GraphicsAllocatorOperation operation,
                             uint64_t first, uint64_t second, uint64_t *result);
