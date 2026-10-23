#include "callback_guard.h"
#include "guest_callback.h"
#include "kernel.h"
#include <stdatomic.h>

static atomic_uint_fast64_t completed;
uint64_t callback_guards_completed(void) { return atomic_load(&completed); }

void callback_mutex_guard(Cpu *cpu, GuestFn original, const CallbackMutexGuard *guard) {
    if (cpu->x[0] > UINT64_MAX - guard->context_pointer_offset ||
        !kernel_access(cpu->x[0] + guard->context_pointer_offset, 8, 1))
        rt_report_adapter_error(cpu, "callback context pointer is inaccessible");
    uint64_t object = LD64(cpu->x[0] + guard->context_pointer_offset);
    if (!object || object > UINT64_MAX - guard->mutex_offset ||
        !kernel_access(object + guard->mutex_offset, guard->mutex_size, 3))
        rt_report_adapter_error(cpu, "callback mutex is inaccessible");
    uint64_t mutex = object + guard->mutex_offset;
    // Helpers run with this guest thread's TLS and stack, preserving the callback
    // arguments/result. No host mutex or changed condition-variable semantics.
    if (!guest_callback(cpu, guard->lock_function, &mutex, 1, NULL))
        rt_report_adapter_error(cpu, "callback mutex lock helper failed");
    original(cpu);
    if (!guest_callback(cpu, guard->unlock_function, &mutex, 1, NULL))
        rt_report_adapter_error(cpu, "callback mutex unlock helper failed");
    atomic_fetch_add(&completed, 1);
}
