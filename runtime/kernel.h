#pragma once
#include "load.h"
#include <stdbool.h>

int kernel_init(const LoadedImage *image, uint64_t stack_base, size_t stack_size, uint32_t *main_handle);
void kernel_destroy(void);
bool kernel_svc(unsigned number, uint64_t *registers);
bool kernel_access(uint64_t address, size_t size, unsigned permissions);
uint32_t kernel_readonly_page(const void *data);
uint32_t kernel_readonly_memory(const void *data, size_t size);
bool kernel_transfer_memory(uint32_t handle, uint64_t *address, uint64_t *size);
