// Explicit adapter policy for a completion callback with a missing mutex guard.
#pragma once
#include "cpu.h"
#include <stdint.h>
typedef struct {
    uint32_t context_pointer_offset, mutex_offset, mutex_size;
    uint64_t lock_function, unlock_function;
} CallbackMutexGuard;

void callback_mutex_guard(Cpu *, GuestFn original, const CallbackMutexGuard *);
uint64_t callback_guards_completed(void);
// Defined by the checked, generated adapter binding.
extern const char callback_guards_sha256[];
_Noreturn void rt_report_adapter_error(Cpu *, const char *message);
