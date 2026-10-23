// Read-only raw RomFS storage. Calls are serialized by the services mutex.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Romfs Romfs;
// A nonzero inherited Windows handle takes precedence over the UTF-8 path.
// It is duplicated, never closed by this function; the caller retains ownership.
Romfs *romfs_open(const char *path, uintptr_t inherited_handle);
uint64_t romfs_size(const Romfs *storage);
bool romfs_read(Romfs *storage, uint64_t offset, void *destination, size_t size);
void romfs_close(Romfs *storage);
