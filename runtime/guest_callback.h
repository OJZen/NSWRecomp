#pragma once
#include "cpu.h"
#include <stdbool.h>

// Synchronous compiled guest re-entry with up to eight integer arguments.
// Only FPSR and the optional x0 result escape the copied Cpu. Guest memory writes
// remain visible. False means an invalid target/argument list or unbalanced SP.
bool guest_callback(Cpu *caller, uint64_t address, const uint64_t *arguments,
                    size_t count, uint64_t *result);
bool guest_callback_address(uint64_t address);
