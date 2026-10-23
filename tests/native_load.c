#include "load.h"
#include "utf8_main.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define BASE 0x7200000000ull
static int deny;
static int has_function(uint64_t address) {
    return !deny && (address == BASE + 0x80 || address == BASE + 0x10000 + 0x80);
}
#ifdef _WIN32
static uintptr_t fault_address;
static LONG CALLBACK trapped(EXCEPTION_POINTERS *exception) {
    EXCEPTION_RECORD *r = exception->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 &&
            r->ExceptionInformation[0] == 1 && r->ExceptionInformation[1] == fault_address)
        ExitProcess(73);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void trapped(int sig) { _exit(sig); }
#endif
int main(int argc, char **argv) {
    assert(argc == 3);
    int mode = atoi(argv[2]);
    deny = mode == 1;
    LoadedImage image = {0};
    if (mode == 2 || mode == 3) {
        uintptr_t guard_base = BASE + (mode == 3 ? 0x10000 : 0);
#ifdef _WIN32
        unsigned char *guard = VirtualAlloc((void *)guard_base, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!guard) return GetLastError() == ERROR_INVALID_ADDRESS ? LOAD_ADDRESS_UNAVAILABLE : LOAD_INVALID;
#elif defined(__APPLE__)
        mach_vm_address_t address = guard_base;
        kern_return_t result = mach_vm_allocate(mach_task_self(), &address, 65536, VM_FLAGS_FIXED);
        if (result == KERN_NO_SPACE) return LOAD_ADDRESS_UNAVAILABLE;
        assert(result == KERN_SUCCESS);
        unsigned char *guard = (void *)(uintptr_t)address;
#else
        unsigned char *guard = mmap((void *)guard_base, 65536, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
#ifndef _WIN32
        if (guard == MAP_FAILED) return LOAD_INVALID;
        if (guard != (void *)guard_base) {
            munmap(guard, 65536);
            return LOAD_ADDRESS_UNAVAILABLE;
        }
#endif
        guard[0] = 73;
        assert(load_plan(argv[1], &image, has_function) == LOAD_ADDRESS_UNAVAILABLE);
        assert(!image.count && guard[0] == 73);
#ifdef _WIN32
        assert(VirtualFree(guard, 0, MEM_RELEASE));
#elif defined(__APPLE__)
        mach_vm_deallocate(mach_task_self(), address, 65536);
#else
        munmap(guard, 65536);
#endif
        assert(!load_plan(argv[1], &image, has_function));
        unload_image(&image);
        return 0;
    }
    int result = load_plan(argv[1], &image, has_function);
    if (result) return result;
    assert(image.count == 2 && image.entry_functions == 1 && image.init_functions == 3);
    unsigned char *first = image.modules[0].memory;
    uint64_t *data = (uint64_t *)(first + image.modules[0].header.data_offset);
    assert(data[0] == BASE + 0x10000 + 0x80);
    assert(data[1] == 32); /* Undefined weak symbol plus a nonzero explicit addend. */
    assert(data[2] == 0x1230);
    assert(data[4] == BASE + 0x90 && data[5] == BASE + 0x94);
    assert(data[8] == BASE + 0x80);
    assert(first[image.modules[0].header.data_offset + image.modules[0].header.data_size] == 0);
    assert(load_plan(argv[1], &image, has_function));
    data[0] = 7;
    assert(data[0] == 7);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(first, &info, sizeof info) == sizeof info && info.Protect == PAGE_READONLY);
    assert(VirtualQuery(data, &info, sizeof info) == sizeof info && info.Protect == PAGE_READWRITE);
    if (mode == 4) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        fault_address = (uintptr_t)first;
        assert(AddVectoredExceptionHandler(1, trapped));
        *(volatile unsigned char *)first = 1;
        return 1;
    }
#else
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGSEGV, trapped);
        signal(SIGBUS, trapped);
        *(volatile unsigned char *)first = 1;
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == SIGSEGV || WEXITSTATUS(status) == SIGBUS);
#endif
    printf("%zu\n", image.mixed_pages);
    unload_image(&image);
    assert(!image.count);
    assert(!load_plan(argv[1], &image, has_function));
    unload_image(&image);
    return 0;
}
