#include "load.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define BASE 0x7200000000ull
static int deny;
static int has_function(uint64_t address) {
    return !deny && (address == BASE + 0x80 || address == BASE + 0x10000 + 0x80);
}
static void trapped(int sig) { _exit(sig); }
int main(int argc, char **argv) {
    assert(argc == 3);
    int mode = atoi(argv[2]);
    deny = mode == 1;
    LoadedImage image = {0};
    if (mode == 2 || mode == 3) {
        uintptr_t guard_base = BASE + (mode == 3 ? 0x10000 : 0);
#ifdef __APPLE__
        mach_vm_address_t address = guard_base;
        assert(mach_vm_allocate(mach_task_self(), &address, 65536, VM_FLAGS_FIXED) == KERN_SUCCESS);
        unsigned char *guard = (void *)(uintptr_t)address;
#else
        unsigned char *guard = mmap((void *)guard_base, 65536, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
        assert(guard == (void *)guard_base);
        guard[0] = 73;
        assert(load_plan(argv[1], &image, has_function));
        assert(!image.count && guard[0] == 73);
#ifdef __APPLE__
        mach_vm_deallocate(mach_task_self(), address, 65536);
#else
        munmap(guard, 65536);
#endif
        assert(!load_plan(argv[1], &image, has_function));
        unload_image(&image);
        return 0;
    }
    if (load_plan(argv[1], &image, has_function)) return 1;
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
    printf("%zu\n", image.mixed_pages);
    unload_image(&image);
    assert(!image.count);
    assert(!load_plan(argv[1], &image, has_function));
    unload_image(&image);
    return 0;
}
