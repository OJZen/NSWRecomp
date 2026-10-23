// Host services used by the portable kernel. Guest memory is never executable.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t host_page_size(void);
// Reserve inaccessible memory wholly below the 39-bit guest address limit.
// Never replace an existing mapping. Release with the original address and size.
void *host_reserve_guest(size_t size);
int host_release(void *address, size_t size);
// Page-aligned ranges belonging to a reservation. Permissions: 0, 1 or 3.
int host_protect(void *address, size_t size, unsigned permission);
// Make pages inaccessible and guarantee zero-filled contents on recommit.
int host_discard(void *address, size_t size);
// Checked copies return false for inaccessible guest pointers.
bool host_read(void *destination, uint64_t source, size_t size);
bool host_write(uint64_t destination, const void *source, size_t size);
bool host_random(void *destination, size_t size);
