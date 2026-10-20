#ifndef TOTK_LOAD_H
#define TOTK_LOAD_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t base, size, text_offset, text_size, ro_offset, ro_size;
    uint64_t data_offset, data_size, bss_size, relocations;
    uint64_t init, init_array, init_array_size, entry, crc32;
} LoadHeader;

typedef struct {
    LoadHeader header;
    void *memory;
    size_t mapped_size;
} LoadedModule;

typedef struct {
    LoadedModule modules[4];
    size_t count, page_size, mixed_pages;
    uint64_t bytes, relocations, init_functions, entry_functions;
} LoadedImage;

/* Plans are temporary developer inputs, not the eventual end-user package format. */
int load_plan(const char *path, LoadedImage *image, int (*has_function)(uint64_t));
void unload_image(LoadedImage *image);
#endif
