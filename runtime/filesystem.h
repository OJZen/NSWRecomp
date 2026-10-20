// Adapted from Zelda-Mobile host/filesystem.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct local_file local_file;
enum { FILESYSTEM_CACHE_COUNT = 8, FILESYSTEM_CACHE_BASE = 2 };
uint32_t filesystem_cache_create(unsigned index, uint64_t size, uint64_t journal);
uint32_t filesystem_cache_info(unsigned index, uint64_t *size);
uint32_t filesystem_cache_delete(unsigned index);
uint32_t filesystem_mount(unsigned volume);
void filesystem_unmount(unsigned volume);
uint32_t filesystem_path(unsigned volume, unsigned command, const char *path, const char *destination,
                         uint64_t argument, local_file **file, uint64_t *value);
uint32_t filesystem_file(local_file *file, unsigned command, uint64_t offset, void *data, size_t size, uint64_t *value);
void filesystem_close(local_file *file);
uint32_t filesystem_commit(unsigned volume);
void filesystem_shutdown(void);
