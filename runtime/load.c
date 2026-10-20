#include "load.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

static int read_u64(FILE *file, uint64_t *value) {
    unsigned char b[8];
    if (fread(b, 1, sizeof b, file) != sizeof b) return 0;
    *value = 0;
    for (unsigned i = 0; i < 8; i++) *value |= (uint64_t)b[i] << (8 * i);
    return 1;
}

static uint64_t memory_u64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc32(const unsigned char *data, size_t size) {
    uint32_t table[256], crc = UINT32_MAX;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t v = i;
        for (unsigned j = 0; j < 8; j++) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1)));
        table[i] = v;
    }
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static int contains(uint64_t start, uint64_t size, uint64_t at, uint64_t length) {
    return at >= start && length <= size && at - start <= size - length;
}

static int overlaps(uint64_t start, uint64_t size, uint64_t page, uint64_t page_size) {
    return size && start < page + page_size && page < start + size;
}

void unload_image(LoadedImage *image) {
    for (size_t i = 0; i < image->count; i++) {
        LoadedModule *m = &image->modules[i];
        if (m->memory) {
#ifdef __APPLE__
            mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)m->memory, m->mapped_size);
#else
            munmap(m->memory, m->mapped_size);
#endif
        }
    }
    memset(image, 0, sizeof *image);
}

static int is_function(const LoadedImage *image, uint64_t address, int (*has_function)(uint64_t)) {
    if (address & 3) return 0;
    for (size_t i = 0; i < image->count; i++) {
        const LoadHeader *h = &image->modules[i].header;
        if (contains(h->base + h->text_offset, h->text_size, address, 4))
            return has_function(address);
    }
    return 0;
}

int load_plan(const char *path, LoadedImage *image, int (*has_function)(uint64_t)) {
    /* Never replace live mappings or silently load the same image twice. */
    if (image->count || !has_function) return 1;
    FILE *file = NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 16 || st.st_size > (256 << 20)) {
        close(fd);
        return 1;
    }
    file = fdopen(fd, "rb");
    if (!file) { close(fd); return 1; }
    char magic[8];
    uint64_t count;
    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096 || page > 65536 || (page & (page - 1))) goto fail;
    image->page_size = (size_t)page;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "TKLOAD1\0", 8) ||
        !read_u64(file, &count) || !count || count > 4) goto fail;

    for (uint64_t i = 0; i < count; i++) {
        LoadedModule *m = &image->modules[i];
        LoadHeader *h = &m->header;
        uint64_t *fields[] = {&h->base, &h->size, &h->text_offset, &h->text_size,
            &h->ro_offset, &h->ro_size, &h->data_offset, &h->data_size, &h->bss_size,
            &h->relocations, &h->init, &h->init_array, &h->init_array_size, &h->entry, &h->crc32};
        for (unsigned k = 0; k < sizeof fields / sizeof *fields; k++)
            if (!read_u64(file, fields[k])) goto fail;
        if (!h->size || h->size > (128 << 20) || h->size % 4096 ||
            h->base < 65536 || h->base % image->page_size ||
            !contains(0, h->size, h->text_offset, h->text_size) || !h->text_size ||
            !contains(0, h->size, h->ro_offset, h->ro_size) ||
            !contains(0, h->size, h->data_offset, h->data_size) ||
            !contains(0, h->size, h->data_offset + h->data_size, h->bss_size) ||
            h->text_offset + h->text_size > h->ro_offset ||
            h->ro_offset + h->ro_size > h->data_offset ||
            h->size - h->data_offset - h->data_size - h->bss_size >= 4096 ||
            h->relocations > (h->data_size + h->bss_size) / 8 || h->crc32 > UINT32_MAX ||
            h->init_array_size % 8 || (h->init_array_size && (h->init_array % 8 ||
            !contains(h->data_offset, h->data_size + h->bss_size, h->init_array, h->init_array_size)))) goto fail;
        m->mapped_size = (h->size + image->page_size - 1) & ~(image->page_size - 1);
        if (h->base > UINTPTR_MAX - m->mapped_size ||
            (i && h->base < image->modules[i - 1].header.base + image->modules[i - 1].mapped_size) ||
            image->bytes + m->mapped_size > (256 << 20)) goto fail;
        void *requested = (void *)(uintptr_t)h->base;
#ifdef __APPLE__
        mach_vm_address_t address = h->base;
        // mmap's address hint may be ignored; FIXED without OVERWRITE fails on collisions.
        if (mach_vm_allocate(mach_task_self(), &address, m->mapped_size, VM_FLAGS_FIXED) != KERN_SUCCESS) goto fail;
        void *mapped = (void *)(uintptr_t)address;
        if (mapped != requested) {
            mach_vm_deallocate(mach_task_self(), address, m->mapped_size);
            goto fail;
        }
#else
        void *mapped = mmap(requested, m->mapped_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) goto fail;
        if (mapped != requested) { munmap(mapped, m->mapped_size); goto fail; }
#endif
        m->memory = mapped;
        image->count++;
        image->bytes += m->mapped_size;
        size_t stored = h->data_offset + h->data_size;
        if (fread(mapped, 1, stored, file) != stored) goto fail;
        unsigned char *bytes = mapped;
        for (size_t j = stored; j < h->size; j++) if (bytes[j]) goto fail;
        uint64_t previous = 0;
        for (uint64_t r = 0; r < h->relocations; r++) {
            uint64_t offset, value;
            if (!read_u64(file, &offset) || !read_u64(file, &value) || offset % 8 ||
                (r && offset <= previous) ||
                !contains(h->data_offset, h->data_size + h->bss_size, offset, 8)) goto fail;
            for (unsigned b = 0; b < 8; b++) bytes[offset + b] = (unsigned char)(value >> (8 * b));
            previous = offset;
        }
        if (crc32(bytes, h->size) != h->crc32) goto fail;
        image->relocations += h->relocations;
    }
    if (fgetc(file) != EOF || ferror(file)) goto fail;
    for (size_t i = 0; i < image->count; i++) {
        LoadedModule *m = &image->modules[i];
        LoadHeader *h = &m->header;
        if (h->init) {
            if (h->init >= h->size || !is_function(image, h->base + h->init, has_function)) goto fail;
            image->init_functions++;
        }
        for (uint64_t at = 0; at < h->init_array_size; at += 8) {
            uint64_t address = memory_u64((unsigned char *)m->memory + h->init_array + at);
            if (!address || address == UINT64_MAX) continue;
            if (!is_function(image, address, has_function)) goto fail;
            image->init_functions++;
        }
        if (h->entry) {
            if (!is_function(image, h->entry, has_function)) goto fail;
            image->entry_functions++;
        }
        for (size_t at = 0; at < m->mapped_size; at += image->page_size) {
            int read = overlaps(h->text_offset, h->text_size, at, image->page_size) ||
                       overlaps(h->ro_offset, h->ro_size, at, image->page_size);
            int write = overlaps(h->data_offset, h->data_size + h->bss_size, at, image->page_size);
            image->mixed_pages += read && write;
            int prot = write ? PROT_READ | PROT_WRITE : read ? PROT_READ : PROT_NONE;
            if (mprotect((unsigned char *)m->memory + at, image->page_size, prot)) goto fail;
        }
    }
    fclose(file);
    return 0;
fail:
    fclose(file);
    unload_image(image);
    fputs("load-only rejected invalid plan, occupied address or missing compiled entry\n", stderr);
    return 1;
}
