#pragma once
#include <stdbool.h>
#include <stdio.h>

FILE *zelda_trace_open(const char *path);
// Call outside the measured window; waits up to five seconds for queued writes.
bool zelda_trace_flush(void);
// Only after all producers have stopped. Fatal signals deliberately do not call this.
int zelda_trace_close(void);
