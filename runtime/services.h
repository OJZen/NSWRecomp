// Adapted from Zelda-Mobile host/services.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool service_connect(const char *name, uint32_t *handle);
bool service_close(uint32_t handle);
bool service_request(uint32_t handle, void *buffer, size_t size, uint32_t *result);
const char *service_blocker(void);
void service_shutdown(void);
