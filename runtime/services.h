// Adapted from Zelda-Mobile host/services.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool service_connect(const char *name, uint32_t *handle);
// Configure client service names before opening any sessions. Comma-separated,
// 1..8 ASCII bytes per name; a trailing '*' matches a name prefix. Empty denies all.
// Basic services require an explicit policy; full mode retains its adapter default.
bool service_set_access(const char *names);
bool service_set_program(uint64_t program);
void service_hid_style_changed(unsigned id);
void service_hid_hold_changed(unsigned value);
bool service_close(uint32_t handle);
bool service_request(uint32_t handle, void *buffer, size_t size, uint32_t *result);
const char *service_blocker(void);
void service_shutdown(void);
