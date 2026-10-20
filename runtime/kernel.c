// Adapted from Zelda-Mobile host/kernel.c; Apache-2.0, see third_party/zelda-mobile/LICENSE.
// Modified for recompiled CPU state and non-executable guest module mappings.
#include "kernel.h"
#include "threads.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>


#define CURRENT_PROCESS 0xffff8001u
#define BAD_HANDLE 0xe401u
#define BAD_COMBINATION 0xe801u
#define MEMORY_LIMIT 0xcd500000ull
#define SYSTEM_RESERVE 0x1800000ull // main.npdm: 24 MiB for memory-management resources.
#define ALIAS_OFFSET 0x10000000ull
#define ALIAS_SIZE (8ull << 30) // The SDK reserves the first 0x3fe00000 bytes for VAMM metadata.
#define STACK_OFFSET (ALIAS_OFFSET + ALIAS_SIZE)
#define STACK_SIZE 0x10000000ull
#define HEAP_OFFSET (STACK_OFFSET + STACK_SIZE)
#define HEAP_CAPACITY (4ull << 30)
#define RESERVE (HEAP_OFFSET + HEAP_CAPACITY)
#define PAGE 0x1000ull

static uint64_t entropy[4];
static const LoadedImage *modules;
static uint8_t *space;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t used_memory;
static uint64_t heap_size;
static uint64_t main_stack_base, main_stack_size;
static uint8_t alias_pages[ALIAS_SIZE / PAGE];
static struct stack_mapping { uint64_t source, destination, size; uint32_t source_state; } stacks[64];
static struct { uint32_t handle; uint8_t *data; size_t size; } readonly_pages[16];
static struct { uint32_t handle; uint64_t address, size; } readonly_maps[16];
static struct { uint64_t base, size; uint32_t permission, attribute; } protections[64];
static size_t nprotections;
static uint32_t next_readonly_handle = 0x200000;
static struct { uint32_t handle, permission; uint64_t address, size; } transfers[16];
static uint32_t next_transfer_handle = 0x300000;
struct memory_info {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count, device_count, padding;
};
_Static_assert(sizeof(struct memory_info) == 40, "Horizon MemoryInfo ABI");

static void region(struct memory_info *info, uint64_t address,
                   uint64_t base, uint64_t size, uint32_t state, uint32_t permission) {
    if (!size || info->state) return;
    if (address >= base && address - base < size) {
        uint64_t lo = base > info->base ? base : info->base;
        uint64_t hi = base + size < info->base + info->size ? base + size : info->base + info->size;
        *info = (struct memory_info){.base = lo, .size = hi - lo, .state = state, .permission = permission};
    } else if (base > address && base < info->base + info->size) {
        info->size = base - info->base;
    } else if (base + size <= address && base + size > info->base) {
        info->size -= base + size - info->base;
        info->base = base + size;
    }
}

static struct memory_info query_memory(uint64_t address) {
    const uint64_t limit = 1ull << 39;
    if (address >= limit) return (struct memory_info){.base = limit, .size = -limit, .state = 16};
    struct memory_info info = {.size = limit};
    if (space) region(&info, address, (uint64_t)space, 0x4000, 12, 3); // Process-local exception context.
    for (size_t i = 0; i < 16; i++) if (readonly_maps[i].handle)
        region(&info, address, readonly_maps[i].address, readonly_maps[i].size, 6, 1);
    for (size_t i = 0; i < 16; i++) if (transfers[i].handle) {
        region(&info, address, transfers[i].address, transfers[i].size, 5, transfers[i].permission);
        if (address >= transfers[i].address && address - transfers[i].address < transfers[i].size) info.attribute |= 1;
    }
    region(&info, address, main_stack_base, main_stack_size, 11, 3);
    for (size_t i = 0; i < 64; i++) if (stacks[i].size) {
        struct stack_mapping *m = &stacks[i];
        region(&info, address, m->destination, m->size, 11, 3);
        if (address >= m->source && address - m->source < m->size)
            return (struct memory_info){.base = m->source, .size = m->size, .state = m->source_state, .attribute = 1};
    }
    region(&info, address, (uint64_t)space + HEAP_OFFSET, heap_size, 5, 3);
    for (size_t i = 0; i < modules->count; i++) {
        const LoadHeader *h = &modules->modules[i].header;
        region(&info, address, h->base + h->text_offset, h->text_size, 3, 5);
        region(&info, address, h->base + h->ro_offset, h->ro_size, 3, 1);
        region(&info, address, h->base + h->data_offset, h->data_size + h->bss_size, 4, 3);
    }
    // Mutex checks in the heap/modules must not scan the entire alias bitmap.
    for (size_t i = 0; !info.state && i < sizeof alias_pages;) {
        const uint8_t *first = memchr(alias_pages + i, 1, sizeof alias_pages - i);
        if (!first) break;
        size_t start = first - alias_pages;
        const uint8_t *end = memchr(first, 0, sizeof alias_pages - start);
        i = end ? (size_t)(end - alias_pages) : sizeof alias_pages;
        region(&info, address, (uint64_t)space + ALIAS_OFFSET + start * PAGE, (i - start) * PAGE, 5, 3);
    }
    if (info.state == 4 || info.state == 5) for (size_t i = 0; i < 64; i++) if (stacks[i].size) {
        uint64_t lo = stacks[i].source, hi = lo + stacks[i].size;
        if (lo > address && lo < info.base + info.size) info.size = lo - info.base;
        if (hi <= address && hi > info.base) { info.size -= hi - info.base; info.base = hi; }
    }
    if (info.state == 4 && !info.attribute) for (size_t i = 0; i < nprotections; i++) {
        uint64_t lo = protections[i].base, hi = lo + protections[i].size;
        if (lo <= address && address < hi) {
            if (protections[i].permission != UINT32_MAX) info.permission = protections[i].permission;
            info.attribute |= protections[i].attribute;
            if (lo > info.base) { info.size -= lo - info.base; info.base = lo; }
            if (hi < info.base + info.size) info.size = hi - info.base;
        } else {
            if (lo > address && lo < info.base + info.size) info.size = lo - info.base;
            if (hi <= address && hi > info.base) { info.size -= hi - info.base; info.base = hi; }
        }
    }
    return info;
}

int kernel_init(const LoadedImage *image, uint64_t stack_base, size_t stack_size, uint32_t *main_handle) {
    modules = image;
    // Horizon exposes four process-lifetime values, not a fresh stream per call.
    arc4random_buf(entropy, sizeof entropy);
    // Keep alias, stack and heap disjoint; reservation consumes virtual addresses, not physical memory.
    // macOS can reserve large low-address holes; ask for a free range above them without replacing mappings.
    mach_vm_address_t address = 0x7000000000ull;
    kern_return_t result = mach_vm_allocate(mach_task_self(), &address, RESERVE, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS) return result == KERN_NO_SPACE ? LOAD_ADDRESS_UNAVAILABLE : LOAD_INVALID;
    if (address > (1ull << 39) - RESERVE) {
        mach_vm_deallocate(mach_task_self(), address, RESERVE);
        return LOAD_ADDRESS_UNAVAILABLE;
    }
    space = (void *)(uintptr_t)address;
    if (mprotect(space, RESERVE, PROT_NONE) || mprotect(space, 0x4000, PROT_READ | PROT_WRITE)) {
        mach_vm_deallocate(mach_task_self(), address, RESERVE);
        space = NULL;
        return LOAD_INVALID;
    }
    main_stack_size = stack_size;
    main_stack_base = stack_base;
    used_memory = image->bytes + main_stack_size;
    *main_handle = threads_init();
    return 0;
}

void kernel_destroy(void) {
    if (!threads_destroy()) return;
    if (space) munmap(space, RESERVE);
    space = NULL;
    heap_size = 0;
    modules = NULL;
    nprotections = 0;
    memset(alias_pages, 0, sizeof alias_pages);
    memset(stacks, 0, sizeof stacks);
    memset(transfers, 0, sizeof transfers);
    for (size_t i = 0; i < 16; i++) free(readonly_pages[i].data);
    memset(readonly_pages, 0, sizeof readonly_pages); memset(readonly_maps, 0, sizeof readonly_maps);
}

uint32_t kernel_readonly_page(const void *data) {
    return kernel_readonly_memory(data, PAGE);
}
bool kernel_transfer_memory(uint32_t handle, uint64_t *address, uint64_t *size) {
    bool found = false;
    pthread_mutex_lock(&memory_lock);
    for (size_t i = 0; i < 16; i++) if (transfers[i].handle == handle && handle) {
        *address = transfers[i].address; *size = transfers[i].size; found = true; break;
    }
    pthread_mutex_unlock(&memory_lock);
    return found;
}
uint32_t kernel_readonly_memory(const void *data, size_t size) {
    if (!data || !size || size % PAGE || size > 0x1100000) return 0;
    pthread_mutex_lock(&memory_lock);
    uint32_t handle = 0;
    if (next_readonly_handle < UINT32_MAX) for (size_t i = 0; i < 16; i++) if (!readonly_pages[i].handle) {
        readonly_pages[i].data = malloc(size);
        if (!readonly_pages[i].data) break;
        memcpy(readonly_pages[i].data, data, size);
        readonly_pages[i].size = size;
        readonly_pages[i].handle = handle = next_readonly_handle++;
        break;
    }
    pthread_mutex_unlock(&memory_lock);
    return handle;
}

bool kernel_access(uint64_t address, size_t size, unsigned permissions) {
    pthread_mutex_lock(&memory_lock);
    struct memory_info info = query_memory(address);
    bool valid = (info.permission & permissions) == permissions && size <= info.size - (address - info.base);
    pthread_mutex_unlock(&memory_lock);
    return valid;
}

static bool readonly_overlap(uint64_t lo, uint64_t hi) {
    size_t page = getpagesize();
    for (size_t i = 0; i < 16; i++) if (readonly_maps[i].handle) {
        uint64_t base = readonly_maps[i].address & ~(uint64_t)(page - 1);
        uint64_t end = (readonly_maps[i].address + readonly_maps[i].size + page - 1) & ~(uint64_t)(page - 1);
        if (lo < end && base < hi) return true;
    }
    return false;
}

static bool memory_svc(unsigned number, uint64_t *r) {
    if (number == 0x15) {
        uint64_t address = r[1], size = r[2]; uint32_t permission = (uint32_t)r[3];
        if (address % PAGE) { r[0] = 0xcc01; return true; }
        if (!size || size % PAGE || size > UINT64_MAX - address) { r[0] = 0xca01; return true; }
        if (permission != 0 && permission != 1 && permission != 3) { r[0] = 0xd801; return true; }
        struct memory_info info = query_memory(address);
        if (info.state != 5 || info.permission != 3 || info.attribute || size > info.size - (address - info.base)) { r[0] = 0xd401; return true; }
        size_t slot = 0; while (slot < 16 && transfers[slot].handle) slot++;
        if (slot == 16 || next_transfer_handle == UINT32_MAX) { r[0] = 0xce01; return true; }
        // Host-page granularity can include adjacent guest allocations; enforce access through the guest memory map.
        transfers[slot].handle = next_transfer_handle++; transfers[slot].address = address;
        transfers[slot].size = size; transfers[slot].permission = permission;
        r[1] = transfers[slot].handle; r[0] = 0; return true;
    }
    if (number == 0x16) for (size_t i = 0; i < 16; i++) if (transfers[i].handle == (uint32_t)r[0] && r[0]) {
        transfers[i].handle = 0; r[0] = 0; return true;
    }
    if (number == 0x03) {
        uint64_t address = r[0], size = r[1]; uint32_t mask = (uint32_t)r[2], attribute = (uint32_t)r[3];
        if (address % PAGE) { r[0] = 0xcc01; return true; }
        if (!size || size % PAGE || size > UINT64_MAX - address) { r[0] = 0xca01; return true; }
        if ((attribute & ~mask) || ((mask & 16) != (attribute & 16))) { r[0] = BAD_COMBINATION; return true; }
        if (mask & ~16u) return false;
        bool already_locked = true;
        for (uint64_t cursor = address; cursor < address + size;) {
            struct memory_info info = query_memory(cursor);
            if (info.state == 5) return false;
            if (info.state != 4 || (info.attribute & 1)) { r[0] = 0xd401; return true; }
            already_locked &= (info.attribute & 16) != 0;
            cursor = info.base + info.size;
        }
        if (mask && !already_locked) {
            if (nprotections == 64) { r[0] = 0xce01; return true; }
            protections[nprotections].base = address; protections[nprotections].size = size;
            protections[nprotections].permission = UINT32_MAX; protections[nprotections++].attribute = 16;
        }
        r[0] = 0; return true;
    }
    if (number == 0x02) {
        uint64_t address = r[0], size = r[1]; unsigned permission = (unsigned)r[2];
        if (address % PAGE) { r[0] = 0xcc01; return true; }
        if (!size || size % PAGE || size > UINT64_MAX - address) { r[0] = 0xca01; return true; }
        if (permission != 0 && permission != 1 && permission != 3) { r[0] = 0xd801; return true; }
        for (uint64_t cursor = address; cursor < address + size;) {
            struct memory_info info = query_memory(cursor);
            if (info.state == 5) return false; // Heap reprotection requires tracking across deallocation.
            if (info.state != 4 || info.attribute) { r[0] = 0xd401; return true; }
            cursor = info.base + info.size;
        }
        size_t index = nprotections;
        for (size_t i = 0; i < nprotections; i++)
            if (protections[i].base == address && protections[i].size == size && protections[i].permission != UINT32_MAX) index = i;
        if (index == 64) { r[0] = 0xce01; return true; }
        // Keep overrides ordered so overlapping later calls replace earlier permissions.
        if (index < nprotections) memmove(protections + index, protections + index + 1, (nprotections - index - 1) * sizeof *protections);
        else nprotections++;
        protections[nprotections - 1].base = address;
        protections[nprotections - 1].size = size;
        protections[nprotections - 1].permission = permission;
        protections[nprotections - 1].attribute = 0;
        size_t host_page = getpagesize();
        uint64_t lo = address & ~(uint64_t)(host_page - 1), hi = (address + size + host_page - 1) & ~(uint64_t)(host_page - 1);
        for (uint64_t page = lo; page < hi; page += host_page) {
            unsigned host_permission = 0;
            for (uint64_t sub = page; sub < page + host_page; sub += PAGE) host_permission |= query_memory(sub).permission;
            // shortcut: mixed 4-KiB permissions share a 16-KiB host page; isolate them with a different memory backend.
            int prot = (host_permission & 1 ? PROT_READ : 0) | (host_permission & 2 ? PROT_WRITE : 0) | (host_permission & 4 ? PROT_EXEC : 0);
            if (mprotect((void *)page, host_page, prot)) { perror("protect module data"); abort(); }
        }
        r[0] = 0; return true;
    }
    if (number == 0x01) {
        uint64_t size = r[1], base = (uint64_t)space + HEAP_OFFSET;
        if (size % 0x200000 || size > HEAP_CAPACITY) { r[0] = 0xca01; return true; }
        if (size > heap_size) {
            uint64_t growth = size - heap_size;
            if (growth > MEMORY_LIMIT - SYSTEM_RESERVE - used_memory) { r[0] = 0x10801; return true; }
            struct memory_info info = query_memory(base + heap_size);
            if (info.state || growth > info.size - (base + heap_size - info.base)) { r[0] = 0xd401; return true; }
            if (mprotect((void *)(base + heap_size), growth, PROT_READ | PROT_WRITE)) { r[0] = 0xd001; return true; }
            used_memory += growth;
        } else if (size < heap_size) {
            for (size_t i = 0; i < 16; i++) if (transfers[i].handle && transfers[i].address < base + heap_size && base + size < transfers[i].address + transfers[i].size) {
                r[0] = 0xd401; return true;
            }
            for (size_t i = 0; i < 64; i++) if (stacks[i].size &&
                stacks[i].source < base + heap_size && base + size < stacks[i].source + stacks[i].size) {
                r[0] = 0xd401; return true;
            }
            // Replace only our released reservation so regrowth is zero-filled without touching gigabytes.
            if (mmap((void *)(base + size), heap_size - size, PROT_NONE,
                     MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) { r[0] = 0xd001; return true; }
            used_memory -= heap_size - size;
        }
        heap_size = size; r[0] = 0; r[1] = base; return true;
    }
    if (number == 0x13 || number == 0x14 || number == 0x16) {
        size_t index = 0;
        while (index < 16 && readonly_pages[index].handle != (uint32_t)r[0]) index++;
        if (!r[0] || index == 16) {
            if (number == 0x16) return false;
            r[0] = BAD_HANDLE; return true;
        }
        if (number == 0x16) { free(readonly_pages[index].data); readonly_pages[index].data = NULL; readonly_pages[index].handle = 0; r[0] = 0; return true; }
        uint64_t address = r[1], base = (uint64_t)space;
        size_t size = readonly_pages[index].size;
        if (address % PAGE) { r[0] = 0xcc01; return true; }
        if (r[2] != size) { r[0] = 0xca01; return true; }
        if (address < base || address - base > RESERVE - size) { r[0] = 0xdc01; return true; }
        size_t host_page = getpagesize();
        uint64_t lo = address & ~(uint64_t)(host_page - 1);
        uint64_t hi = (address + size + host_page - 1) & ~(uint64_t)(host_page - 1);
        if (number == 0x13) {
            if (r[3] != 1) { r[0] = 0xd801; return true; }
            struct memory_info info = query_memory(address);
            if (info.state || size > info.size - (address - info.base)) { r[0] = 0xd401; return true; }
            for (uint64_t page = lo; page < hi; page += PAGE) {
                if (page >= address && page < address + size) continue;
                struct memory_info neighbor = query_memory(page);
                if (neighbor.state && neighbor.state != 6) { r[0] = 0xd401; return true; }
            }
            size_t slot = 0; while (slot < 16 && readonly_maps[slot].handle) slot++;
            if (slot == 16) { r[0] = 0xce01; return true; }
            // shortcut: immutable clocks/fonts use snapshots; mutable devices need shared backing.
            if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) { r[0] = 0xd001; return true; }
            memcpy((void *)address, readonly_pages[index].data, size);
            if (mprotect((void *)lo, hi - lo, PROT_READ)) { perror("protect shared memory"); abort(); }
            readonly_maps[slot].handle = (uint32_t)r[0]; readonly_maps[slot].address = address;
            readonly_maps[slot].size = size;
        } else {
            size_t slot = 0;
            while (slot < 16 && (readonly_maps[slot].handle != (uint32_t)r[0] || readonly_maps[slot].address != address)) slot++;
            if (slot == 16) { r[0] = 0xd401; return true; }
            readonly_maps[slot].handle = 0;
            for (uint64_t page = lo; page < hi; page += host_page)
                if (!readonly_overlap(page, page + host_page) && (mprotect((void *)page, host_page, PROT_NONE) || madvise((void *)page, host_page, MADV_FREE))) {
                    perror("release shared memory"); abort();
                }
        }
        r[0] = 0; return true;
    }
    if (number == 0x04 || number == 0x05) {
        uint64_t dst = r[0], src = r[1], size = r[2], base = (uint64_t)space + STACK_OFFSET;
        if (dst % PAGE || src % PAGE) { r[0] = 0xcc01; return true; }
        if (!size || size % PAGE || size > UINT64_MAX - src) { r[0] = 0xca01; return true; }
        if (dst < base || dst - base >= STACK_SIZE || size > STACK_SIZE - (dst - base)) {
            r[0] = 0xdc01; return true;
        }
        size_t host_page = getpagesize();
        uint64_t lo = dst & ~(uint64_t)(host_page - 1), hi = (dst + size + host_page - 1) & ~(uint64_t)(host_page - 1);
        if (readonly_overlap(lo, hi)) { r[0] = 0xd401; return true; }
        if (number == 0x04) {
            struct memory_info source = query_memory(src);
            if ((source.state != 4 && source.state != 5) || source.permission != 3 || source.attribute ||
                size > source.size - (src - source.base)) { r[0] = 0xd401; return true; }
            struct stack_mapping *slot = NULL;
            for (size_t i = 0; i < 64; i++) {
                struct stack_mapping *m = &stacks[i];
                if (!m->size) { if (!slot) slot = m; continue; }
                if (dst < m->destination + m->size && m->destination < dst + size) { r[0] = 0xd401; return true; }
                if (src < m->source + m->size && m->source < src + size) { r[0] = 0xd401; return true; }
            }
            if (!slot) { r[0] = 0xce01; return true; }
            if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) { r[0] = 0xd001; return true; }
            mach_vm_size_t copied = 0;
            if (mach_vm_read_overwrite(mach_task_self(), src, size, dst, &copied) || copied != size) {
                fputs("stack source changed during mapping\n", stderr); abort();
            }
            // shortcut: copy-back models stack ownership; 4-KiB source isolation needs a different memory backend.
            *slot = (struct stack_mapping){src, dst, size, source.state};
        } else {
            struct stack_mapping *slot = NULL;
            for (size_t i = 0; i < 64; i++) if (stacks[i].destination == dst && stacks[i].source == src && stacks[i].size == size) slot = &stacks[i];
            if (!slot) { r[0] = 0xd401; return true; }
            if (mach_vm_write(mach_task_self(), src, (vm_offset_t)dst, (mach_msg_type_number_t)size)) {
                fputs("stack source changed during unmapping\n", stderr); abort();
            }
            memset(slot, 0, sizeof *slot);
            for (uint64_t page = lo; page < hi; page += host_page) {
                bool live = false;
                for (size_t i = 0; i < 64; i++) if (stacks[i].size)
                    live |= page < stacks[i].destination + stacks[i].size && stacks[i].destination < page + host_page;
                if (!live && (mprotect((void *)page, host_page, PROT_NONE) || madvise((void *)page, host_page, MADV_FREE))) {
                    perror("release guest stack"); abort();
                }
            }
        }
        r[0] = 0; return true;
    }
    if (number == 0x2c || number == 0x2d) {
        uint64_t address = r[0], size = r[1], base = (uint64_t)space + ALIAS_OFFSET;
        if (address % PAGE) { r[0] = 0xcc01; return true; }
        if (!size || size % PAGE) { r[0] = 0xca01; return true; }
        if (address < base || address - base >= ALIAS_SIZE || size > ALIAS_SIZE - (address - base)) {
            r[0] = 0xdc01; return true;
        }
        if (number == 0x2d) for (size_t i = 0; i < 64; i++) if (stacks[i].size &&
                address < stacks[i].source + stacks[i].size && stacks[i].source < address + size) {
            r[0] = 0xd401; return true;
        }
        size_t first = (address - base) / PAGE, count = size / PAGE, changed = 0;
        for (size_t i = first; i < first + count; i++) changed += number == 0x2c ? !alias_pages[i] : alias_pages[i];
        size_t host_page = (size_t)getpagesize();
        uint64_t lo = address & ~(uint64_t)(host_page - 1), hi = (address + size + host_page - 1) & ~(uint64_t)(host_page - 1);
        if (readonly_overlap(lo, hi)) { r[0] = 0xd401; return true; }
        if (number == 0x2c) {
            if (changed * PAGE > MEMORY_LIMIT - SYSTEM_RESERVE - used_memory) { r[0] = 0x10801; return true; }
            if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) { r[0] = 0xd001; return true; }
            for (size_t i = first; i < first + count; i++) if (!alias_pages[i]) {
                memset((void *)(base + i * PAGE), 0, PAGE);
                alias_pages[i] = 1;
            }
            used_memory += changed * PAGE;
        } else {
            memset(alias_pages + first, 0, count);
            // A 16-KiB host page can contain four independent 4-KiB guest mappings.
            // shortcut: protection is host-page-granular; per-4-KiB fault isolation needs a different memory backend.
            for (uint64_t page = lo; page < hi; page += host_page) {
                bool live = false;
                for (size_t i = (page - base) / PAGE; i < (page - base + host_page) / PAGE; i++) live |= alias_pages[i];
                if (!live && (mprotect((void *)page, host_page, PROT_NONE) || madvise((void *)page, host_page, MADV_FREE))) {
                    perror("release guest page"); exit(1);
                }
            }
            used_memory -= changed * PAGE;
        }
        r[0] = 0; return true;
    }
    if (number == 0x06) {
        struct memory_info info = query_memory(r[2]);
        // The kernel must reject invalid output pointers instead of faulting in memcpy.
        r[0] = mach_vm_write(mach_task_self(), r[0], (vm_offset_t)&info, sizeof info) == KERN_SUCCESS ? 0 : 0xe601;
        r[1] = 0;
        return true;
    }
    if (number != 0x29) return false;
    uint64_t type = r[1], subtype = r[3], value = 0;
    if (type == 11) {
        r[0] = (uint32_t)r[2] ? BAD_HANDLE : subtype >= 4 ? BAD_COMBINATION : 0;
        r[1] = r[0] ? 0 : entropy[subtype];
        return true;
    }
    switch (type) {
        case 0: value = 7; break; // main.npdm permits virtual cores 0..2.
        case 1: value = (1ull << 60) - (1ull << 28); break;
        case 2: value = (uint64_t)space + ALIAS_OFFSET; break;
        case 3: value = ALIAS_SIZE; break;
        case 4: value = (uint64_t)space + HEAP_OFFSET; break;
        case 5: value = HEAP_CAPACITY; break;
        case 6: value = MEMORY_LIMIT; break;
        case 7: value = used_memory + SYSTEM_RESERVE; break;
        case 12: value = (uint64_t)space; break;
        case 13: value = RESERVE; break;
        case 14: value = (uint64_t)space + STACK_OFFSET; break;
        case 15: value = STACK_SIZE; break;
        case 16: value = SYSTEM_RESERVE; break;
        case 20: value = (uint64_t)space; break;
        case 21: value = MEMORY_LIMIT - SYSTEM_RESERVE; break;
        case 22: value = used_memory; break;
        case 28: value = 0; break; // No extra alias reservation in this address space.
        default: return false;
    }
    r[0] = subtype ? BAD_COMBINATION : (uint32_t)r[2] != CURRENT_PROCESS ? BAD_HANDLE : 0;
    r[1] = r[0] ? 0 : value;
    return true;
}

bool kernel_svc(unsigned number, uint64_t *r) {
    if (threads_svc(number, r)) return true;
    pthread_mutex_lock(&memory_lock);
    bool handled = memory_svc(number, r);
    pthread_mutex_unlock(&memory_lock);
    return handled;
}
