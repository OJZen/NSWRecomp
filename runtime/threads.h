#pragma once
#include <stdbool.h>
#include <stdint.h>

uint32_t threads_init(void);
bool threads_destroy(void);
bool threads_svc(unsigned number, uint64_t *registers);
void threads_checkpoint(void);
bool threads_stop(void);
