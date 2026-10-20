#include "kernel.h"
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc, device, padding;
} MemoryInfo;

static uint64_t info(unsigned type) {
    uint64_t r[32] = {0, type, 0xffff8001u};
    assert(kernel_svc(0x29, r) && !r[0]);
    return r[1];
}

static MemoryInfo query(uint64_t address) {
    struct { MemoryInfo value; uint64_t guard; } out = {.guard = 0x12345678};
    uint64_t r[32] = {(uintptr_t)&out, 0, address};
    assert(kernel_svc(6, r) && !r[0] && !r[1]);
    assert(out.guard == 0x12345678);
    return out.value;
}

int main(void) {
    alarm(10);
    unsigned char *memory = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    LoadedImage image = {.count = 1, .bytes = 0x10000};
    image.modules[0].header = (LoadHeader){.base = (uintptr_t)memory, .size = 0x10000,
        .text_size = 0xf00, .ro_offset = 0x4000, .ro_size = 0x800,
        .data_offset = 0x8000, .data_size = 0x1000, .bss_size = 0x1000};
    uint32_t handle;
    int result = kernel_init(&image, (uintptr_t)memory + 0x10000, 0x10000, &handle);
    if (result) { munmap(memory, 0x20000); return result; }
    assert(query((uintptr_t)memory).permission == 5);
    assert(query((uintptr_t)memory + 0x4000).permission == 1);
    assert(query((uintptr_t)memory + 0xfff).size == 0x1000);
    assert(query((uintptr_t)memory + 0xfff).state == 3);
    assert(query((uintptr_t)memory + 0x4fff).permission == 1);
    assert(!query((uintptr_t)memory + 0x5000).state);
    assert(query((uintptr_t)memory + 0x9000).state == 4);
    assert(query((uintptr_t)memory + 0x10000).state == 11);
    assert(kernel_access((uintptr_t)memory + 0x8000, 0x2000, 3));
    assert(!kernel_access((uintptr_t)memory + 0x8000, 0x2001, 3));
    uint64_t r[32] = {1, 0, 0};
    assert(kernel_svc(6, r) && r[0] == 0xe601);
    for (unsigned i = 0; i < 4; i++) {
        r[0] = 0; r[1] = 11; r[2] = 0; r[3] = i;
        assert(kernel_svc(0x29, r) && !r[0]);
        uint64_t value = r[1]; r[1] = 11;
        assert(kernel_svc(0x29, r) && !r[0] && r[1] == value);
    }
    r[1] = 11; r[2] = 1; r[3] = 0;
    assert(kernel_svc(0x29, r) && r[0] == 0xe401);
    r[1] = 11; r[2] = 0; r[3] = 4;
    assert(kernel_svc(0x29, r) && r[0] == 0xe801);
    r[1] = handle;
    assert(kernel_svc(0x0c, r) && !r[0] && r[1] == 44);
    r[1] = handle;
    assert(kernel_svc(0x25, r) && !r[0] && r[1]);
    r[0] = handle; r[1] = 60;
    assert(kernel_svc(0x0d, r) && r[0] == 0xe001);
    r[1] = 0x8002;
    assert(kernel_svc(0x0c, r) && r[0] == 0xe401);
    r[0] = handle; r[1] = (uint32_t)-1; r[2] = 4;
    assert(kernel_svc(0x0f, r) && !r[0]);
    r[2] = handle;
    assert(kernel_svc(0x0e, r) && !r[0] && r[1] == UINT32_MAX && r[2] == 4);
    assert(kernel_svc(0x10, r) && r[0] == 2);
    uint64_t before = info(22), heap = info(4), alias = info(2), stack = info(14);
    assert(!(info(12) % 0x200000) && !(heap % 0x200000) && !(alias % 0x200000) && !(stack % 0x200000));
    r[1] = 0x200000;
    assert(kernel_svc(1, r) && !r[0] && r[1] == heap);
    assert(info(22) == before + 0x200000 && query(heap).state == 5);
    *(uint64_t *)heap = 123;
    r[1] = 0; assert(kernel_svc(1, r) && !r[0]);
    r[1] = 0x200000; assert(kernel_svc(1, r) && !r[0] && !*(uint64_t *)heap);
    r[0] = alias; r[1] = 0x10000;
    assert(kernel_svc(0x2c, r) && !r[0]);
    *(uint64_t *)alias = 456;
    r[0] = stack; r[1] = alias; r[2] = 0x10000;
    assert(kernel_svc(4, r) && !r[0]);
    assert(query(alias).attribute == 1 && query(stack).state == 11 && *(uint64_t *)stack == 456);
    *(uint64_t *)stack = 789;
    r[0] = stack; r[1] = alias; r[2] = 0x10000;
    assert(kernel_svc(5, r) && !r[0] && *(uint64_t *)alias == 789);
    assert(!query(stack).state && !query(alias).attribute);
    r[0] = alias; r[1] = 0x10000;
    assert(kernel_svc(0x2d, r) && !r[0] && !query(alias).state);
    assert(!kernel_svc(0x7f, r));
    memset(r, 0, sizeof r);
    assert(kernel_svc(8, r) && r[0] == 0xcc01);
    kernel_destroy();
    munmap(memory, 0x20000);
    return 0;
}
