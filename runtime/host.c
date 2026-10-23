// Mach reservation/copy operations adapted from Zelda-Mobile host/kernel.c;
// Apache-2.0, see third_party/zelda-mobile/LICENSE. Windows backend is local.
#include "host.h"
#include <errno.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include "mach_memory.h"
#endif

size_t host_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); return info.dwPageSize;
#else
    return (size_t)getpagesize();
#endif
}

void *host_reserve_guest(size_t size) {
    const uint64_t hint = 0x7000000000ull, limit = 1ull << 39;
    if (!size || size >= limit) return NULL;
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info);
    uint64_t granularity = info.dwAllocationGranularity;
    // Query free regions instead of requesting a specific colliding address or
    // accepting a normal Windows allocation outside the guest address space.
    for (unsigned pass = 0; pass < 2; pass++) {
        uint64_t cursor = pass ? granularity : hint, end = pass ? hint : limit;
        while (cursor < end && size <= end - cursor) {
            MEMORY_BASIC_INFORMATION region;
            if (!VirtualQuery((void *)(uintptr_t)cursor, &region, sizeof region)) break;
            uint64_t next = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
            if (next <= cursor) break;
            if (region.State == MEM_FREE && size <= next - cursor) {
                void *memory = VirtualAlloc((void *)(uintptr_t)cursor, size, MEM_RESERVE, PAGE_NOACCESS);
                if (memory) return memory;
            }
            cursor = (next + granularity - 1) & ~(granularity - 1);
        }
    }
    return NULL;
#else
    mach_vm_address_t address = hint;
    if (mach_vm_allocate(mach_task_self(), &address, size, VM_FLAGS_ANYWHERE)) return NULL;
    if (address > limit - size || mprotect((void *)(uintptr_t)address, size, PROT_NONE)) {
        mach_vm_deallocate(mach_task_self(), address, size);
        return NULL;
    }
    return (void *)(uintptr_t)address;
#endif
}

int host_release(void *address, size_t size) {
#ifdef _WIN32
    (void)size;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
#else
    return munmap(address, size);
#endif
}

static bool pages(void *address, size_t size) {
    size_t page = host_page_size();
    return address && size && !((uintptr_t)address % page) && !(size % page) &&
        size <= UINTPTR_MAX - (uintptr_t)address;
}

int host_protect(void *address, size_t size, unsigned permission) {
    if (!pages(address, size) || (permission != 0 && permission != 1 && permission != 3)) {
        errno = EINVAL; return -1;
    }
#ifdef _WIN32
    DWORD protection = permission == 3 ? PAGE_READWRITE : permission ? PAGE_READONLY : PAGE_NOACCESS;
    // Committing an already committed page preserves its contents. VirtualProtect
    // is still required to change the protection of those existing pages.
    if (!VirtualAlloc(address, size, MEM_COMMIT, protection)) return -1;
    DWORD previous;
    return VirtualProtect(address, size, protection, &previous) ? 0 : -1;
#else
    int protection = (permission & 1 ? PROT_READ : 0) | (permission & 2 ? PROT_WRITE : 0);
    return mprotect(address, size, protection);
#endif
}

int host_discard(void *address, size_t size) {
    if (!pages(address, size)) { errno = EINVAL; return -1; }
#ifdef _WIN32
    return VirtualFree(address, size, MEM_DECOMMIT) ? 0 : -1;
#else
    // Only callers owning this reservation may replace the released pages.
    return mmap(address, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED ? -1 : 0;
#endif
}

bool host_read(void *destination, uint64_t source, size_t size) {
#ifdef _WIN32
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)source, destination, size, &copied) && copied == size;
#else
    mach_vm_size_t copied = 0;
    return !mach_vm_read_overwrite(mach_task_self(), source, size, (mach_vm_address_t)destination, &copied) && copied == size;
#endif
}

bool host_write(uint64_t destination, const void *source, size_t size) {
    if (size > UINT64_MAX - destination) return false;
#ifdef _WIN32
    // WriteProcessMemory is a debugger API; explicitly reject read-only memory
    // before using it, rather than relying on its protection-changing behavior.
    for (uint64_t cursor = destination; cursor < destination + size;) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((void *)(uintptr_t)cursor, &region, sizeof region) || region.State != MEM_COMMIT ||
                (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY)) return false;
        uint64_t next = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    SIZE_T copied = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)destination, source, size, &copied) && copied == size;
#else
    if (size > UINT32_MAX) return false;
    return !mach_vm_write(mach_task_self(), destination, (vm_offset_t)source, (mach_msg_type_number_t)size);
#endif
}

bool host_random(void *destination, size_t size) {
#ifdef _WIN32
    return size <= ULONG_MAX && BCryptGenRandom(NULL, destination, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    arc4random_buf(destination, size);
    return true;
#endif
}
