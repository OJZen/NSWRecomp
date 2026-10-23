#pragma once
#include <stdbool.h>
#include <stdint.h>

uint32_t threads_init(void);
bool threads_destroy(void);
bool threads_svc(unsigned number, uint64_t *registers);
void threads_checkpoint(void);
// Diagnostic snapshots only; they never resume or alter a guest wait.
void threads_trace(uint64_t address, uint64_t lr, uint64_t x0);
void threads_dump(void);
bool threads_stop(void);
void threads_request_stop(void);
