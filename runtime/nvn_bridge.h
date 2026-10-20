#pragma once
#include "cpu.h"

typedef struct { const char *name; GuestFn call; } NvnBridgeEntry;
extern const NvnBridgeEntry nvn_bridge_entries[];
extern const size_t nvn_bridge_count;
extern _Thread_local Cpu *nvn_bridge_cpu;

void *nvn_bridge_native(size_t index);
uint64_t nvn_bridge_stack(Cpu *cpu, size_t offset);
void nvn_bridge_unavailable(Cpu *cpu, const char *name) __attribute__((noreturn));
