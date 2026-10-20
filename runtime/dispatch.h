#pragma once
#include "cpu.h"

GuestFn rt_find_function(uint64_t address);
GuestFn rt_find_external(uint64_t address);
void rt_report_missing(Cpu *cpu);
int rt_validate_tables(void);
