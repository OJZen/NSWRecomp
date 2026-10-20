#pragma once
#include "cpu.h"

GuestFn rt_find_function(uint64_t address);
GuestFn rt_find_external(uint64_t address);
int rt_validate_tables(void);
