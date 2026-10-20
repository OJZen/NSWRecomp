#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include "kernel.h"

#define LOG(...) do { fprintf(stderr, "[graphics] "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
void die(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));
#define HLE_FORCE(symbol, why, ret, name, ...) static ret name(__VA_ARGS__)
uint64_t now_ns(void);
uint64_t guest_ns(void);
int dm_detclock_enabled(void);
void dm_guest_frame_tick(void);
void *front_metal_layer(void);
void crash_set_wait_dump(void (*fn)(void));
void dm_res_report_register(void (*fn)(uint32_t, const char *));
void dm_fault_hook_register(int (*fn)(void *, int, void *));
void nvn_guest_callback(uint64_t function, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
