#include "load.h"
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
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
#if defined(_WIN32)
            VirtualFree(m->memory, 0, MEM_RELEASE);
#elif defined(__APPLE__)
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
#ifdef _WIN32
    // Open the reparse point itself and reject it, matching O_NOFOLLOW. Binary
    // mode is essential: plans contain bytes which Windows text I/O rewrites.
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!length) return LOAD_INVALID;
    wchar_t *wide = malloc((size_t)length * sizeof *wide);
    if (!wide) return LOAD_INVALID;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length)) {
        free(wide); return LOAD_INVALID;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return LOAD_INVALID;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(handle); return LOAD_INVALID;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); return LOAD_INVALID; }
#else
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
#endif
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
    const char *stage = "header";
    size_t module_index = 0;
    int native_error = 0;
    int result = LOAD_INVALID;
#ifdef _WIN32
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    long page = system.dwPageSize;
#else
    long page = sysconf(_SC_PAGESIZE);
#endif
    if (page < 4096 || page > 65536 || (page & (page - 1))) goto fail;
    image->page_size = (size_t)page;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "TKLOAD1\0", 8) ||
        !read_u64(file, &count) || !count || count > LOAD_MAX_MODULES) goto fail;

    for (uint64_t i = 0; i < count; i++) {
        module_index = i;
        stage = "module layout";
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
        stage = "address reservation";
#ifdef _WIN32
        if (h->base % system.dwAllocationGranularity) goto fail;
        void *mapped = VirtualAlloc(requested, m->mapped_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!mapped) {
            native_error = (int)GetLastError();
            if (native_error == ERROR_INVALID_ADDRESS) result = LOAD_ADDRESS_UNAVAILABLE;
            goto fail;
        }
        if (mapped != requested) {
            VirtualFree(mapped, 0, MEM_RELEASE);
            result = LOAD_ADDRESS_UNAVAILABLE;
            goto fail;
        }
#elif defined(__APPLE__)
        mach_vm_address_t address = h->base;
        // mmap's address hint may be ignored; FIXED without OVERWRITE fails on collisions.
        native_error = mach_vm_allocate(mach_task_self(), &address, m->mapped_size, VM_FLAGS_FIXED);
        if (native_error != KERN_SUCCESS) {
            if (native_error == KERN_NO_SPACE) result = LOAD_ADDRESS_UNAVAILABLE;
            goto fail;
        }
        void *mapped = (void *)(uintptr_t)address;
        if (mapped != requested) {
            mach_vm_deallocate(mach_task_self(), address, m->mapped_size);
            result = LOAD_ADDRESS_UNAVAILABLE;
            goto fail;
        }
#else
        void *mapped = mmap(requested, m->mapped_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) { native_error = errno; goto fail; }
        if (mapped != requested) {
            munmap(mapped, m->mapped_size);
            result = LOAD_ADDRESS_UNAVAILABLE;
            goto fail;
        }
#endif
        m->memory = mapped;
        image->count++;
        image->bytes += m->mapped_size;
        stage = "segment copy";
        size_t stored = h->data_offset + h->data_size;
        if (fread(mapped, 1, stored, file) != stored) goto fail;
        unsigned char *bytes = mapped;
        for (size_t j = stored; j < h->size; j++) if (bytes[j]) goto fail;
        uint64_t previous = 0;
        stage = "relocation writes";
        for (uint64_t r = 0; r < h->relocations; r++) {
            uint64_t offset, value;
            if (!read_u64(file, &offset) || !read_u64(file, &value) || offset % 8 ||
                (r && offset <= previous) ||
                !contains(h->data_offset, h->data_size + h->bss_size, offset, 8)) goto fail;
            for (unsigned b = 0; b < 8; b++) bytes[offset + b] = (unsigned char)(value >> (8 * b));
            previous = offset;
        }
        stage = "memory checksum";
        if (crc32(bytes, h->size) != h->crc32) goto fail;
        image->relocations += h->relocations;
    }
    stage = "file end";
    if (fgetc(file) != EOF || ferror(file)) goto fail;
    for (size_t i = 0; i < image->count; i++) {
        module_index = i;
        stage = "compiled entry coverage";
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
            stage = "page protection";
            int read = overlaps(h->text_offset, h->text_size, at, image->page_size) ||
                       overlaps(h->ro_offset, h->ro_size, at, image->page_size);
            int write = overlaps(h->data_offset, h->data_size + h->bss_size, at, image->page_size);
            image->mixed_pages += read && write;
#ifdef _WIN32
            DWORD old, prot = write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
            if (!VirtualProtect((unsigned char *)m->memory + at, image->page_size, prot, &old)) {
                native_error = (int)GetLastError();
#else
            int prot = write ? PROT_READ | PROT_WRITE : read ? PROT_READ : PROT_NONE;
            if (mprotect((unsigned char *)m->memory + at, image->page_size, prot)) {
                native_error = errno;
#endif
                goto fail;
            }
        }
    }
    fclose(file);
    return 0;
fail:
    fclose(file);
    unload_image(image);
    fprintf(stderr, "load-only rejected: %s; module=%zu; native=%d\n", stage, module_index, native_error);
    return result;
}
