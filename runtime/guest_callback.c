#include "guest_callback.h"
#include "dispatch.h"

bool guest_callback_address(uint64_t address) {
    // Host API tokens are not compiled guest callbacks.
    return address && !(address & 3) && !rt_find_external(address) && rt_find_function(address);
}

bool guest_callback(Cpu *caller, uint64_t address, const uint64_t *arguments,
                    size_t count, uint64_t *result) {
    if (!caller || (caller->sp & 15) || count > 8 || (count && !arguments) ||
        !guest_callback_address(address)) return false;
    Cpu callback = *caller;
    callback.pc = address;
    callback.x[30] = 0;
    callback.excl_addr = UINT64_MAX;
    callback.excl_size = 0;
    if (count) memcpy(callback.x, arguments, count * sizeof *arguments);
    rt_dispatch(&callback);
    if (callback.sp != caller->sp) return false;
    caller->fpsr = callback.fpsr;
    if (result) *result = callback.x[0];
    return true;
}
