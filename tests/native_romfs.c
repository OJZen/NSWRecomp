// Synthetic raw storage only; no game data.
#include "romfs.h"
#include "utf8_main.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    uintptr_t inherited = 0;
    const char *value = getenv("NSW_ROMFS_HANDLE");
    if (value) inherited = (uintptr_t)strtoull(value, NULL, 10);
    Romfs *storage = romfs_open(argv[1], inherited);
    if (!strcmp(argv[2], "reject")) { assert(!storage); return 0; }
    assert(storage);
    uint64_t size = strtoull(argv[2], NULL, 10);
    assert(romfs_size(storage) == size);
    uint64_t header = 0;
    assert(romfs_read(storage, 0, &header, 8) && header == 0x50);
    uint8_t *bytes = malloc(0x20000); assert(bytes);
    if (size < UINT32_MAX) {
        assert(romfs_read(storage, 0x200, bytes, 0x20000));
        for (size_t i = 0; i < 0x20000; i++) assert(bytes[i] == (uint8_t)i);
    }
    assert(romfs_read(storage, size - 64, bytes, 64));
    for (size_t i = 0; i < 64; i++) assert(bytes[i] == (uint8_t)(size - 64 + i));
    memset(bytes, 0xa5, 64);
    assert(!romfs_read(storage, size - 1, bytes, 2) && bytes[0] == 0xa5);
    assert(!romfs_read(storage, UINT64_MAX, bytes, 64));
    assert(!romfs_read(storage, 0, NULL, 1));
    assert(romfs_read(storage, size, NULL, 0));
    puts("ready"); fflush(stdout);
    if (getenv("NSW_TEST_HOLD")) assert(getchar() == '\n');
    romfs_close(storage); romfs_close(NULL); free(bytes);
    puts("RomFS storage checks passed");
    return 0;
}
