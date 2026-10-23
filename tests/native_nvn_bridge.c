#include "nvn_bridge.h"
#include "nvn_protos.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bool_missing;
static unsigned invoked;
static uint64_t wide(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                     uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    assert(nvn_bridge_cpu && nvn_bridge_cpu->x[7] == 8);
    assert(a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && i == 9 && j == 10);
    invoked++; return 0xfedcba9876543210ull;
}
static float floating(float a, float b, float c, float d, float e,
                      float f, float g, float h, float i, float j) {
    assert(a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && i == 9 && j == 10);
    invoked++; return a + b + c + d + e + f + g + h + i + j;
}
static uint64_t mixed(uint64_t a, float b, uint64_t c, float d, uint64_t e, float f,
                      uint64_t g, float h, uint64_t i, float j, uint64_t k, float l,
                      uint64_t m, float n, uint64_t o, float p, uint64_t q, float r) {
    assert(a == 10 && c == 20 && e == 30 && g == 40 && i == 50 && k == 60 && m == 70 && o == 80 && q == 90);
    assert(b == .25f && d == 1.25f && f == 2.25f && h == 3.25f && j == 4.25f && l == 5.25f && n == 6.25f && p == 7.25f && r == 8.25f);
    invoked++; return 0x1122334455667788ull;
}
static NVNseparateTextureHandle small(NVNseparateTextureHandle value, int change) {
    assert(value.value == 0xfedcba98u && change == -3);
    invoked++; return (NVNseparateTextureHandle){value.value - 3};
}
static NVNseparateSamplerHandle handle64(NVNseparateSamplerHandle value) {
    assert(value.value == 0xfedcba9876543210ull); invoked++; return value;
}
static const void *pointer(uint32_t *value, int change) { assert(change == -7); *value = (uint32_t)change; invoked++; return value; }
static NVNmode enum_value(int value) { assert(value == -2); invoked++; return (NVNmode)value; }
static PFNNVNCALLBACKPROC callback(PFNNVNCALLBACKPROC value) { invoked++; return value; }
static void marker(void) { abort(); }

void *nvn_bridge_native(size_t index) {
    assert(index < nvn_bridge_count);
    if (bool_missing) return NULL;
    const char *name = nvn_bridge_entries[index].name;
#define NATIVE(fn) if (!strcmp(name, #fn)) return (void *)fn
    NATIVE(wide); NATIVE(floating); NATIVE(mixed); NATIVE(small); NATIVE(handle64); NATIVE(pointer); NATIVE(enum_value); NATIVE(callback);
    exit(99); // Unsupported ABI must stop before looking up a native function.
}
uint64_t nvn_bridge_stack(Cpu *cpu, size_t offset) { assert(offset <= 8); return LD64(cpu->sp + offset); }
void nvn_bridge_unavailable(Cpu *cpu, const char *name) { (void)cpu; (void)name; exit(78); }
static void call(const char *name, Cpu *cpu) {
    Cpu previous = {0}; nvn_bridge_cpu = &previous;
    for (size_t i = 0; i < nvn_bridge_count; i++) if (!strcmp(nvn_bridge_entries[i].name, name)) {
        nvn_bridge_entries[i].call(cpu);
        assert(nvn_bridge_cpu == &previous); nvn_bridge_cpu = NULL; return;
    }
    abort();
}

int main(int argc, char **argv) {
    Cpu cpu = {0};
    _Alignas(16) uint64_t stack[4] = {9, 10}; cpu.sp = (uintptr_t)stack;
    if (argc == 2) {
        bool_missing = !strcmp(argv[1], "missing");
        call(bool_missing ? "wide" : "unsupported", &cpu);
        return 98;
    }
    for (unsigned i = 0; i < 8; i++) cpu.x[i] = i + 1;
    call("wide", &cpu); assert(cpu.x[0] == 0xfedcba9876543210ull);
    for (unsigned i = 0; i < 8; i++) { float value = i + 1; cpu.v[i] = UINT64_MAX; memcpy(&cpu.v[i], &value, 4); }
    float nine = 9, ten = 10; memcpy(stack, &nine, 4); memcpy(stack + 1, &ten, 4);
    call("floating", &cpu); float result; memcpy(&result, &cpu.v[0], 4);
    assert(result == 55 && !(cpu.v[0] >> 32));
    for (unsigned i = 0; i < 8; i++) { float value = i + .25f; cpu.x[i] = (i + 1) * 10; memcpy(&cpu.v[i], &value, 4); }
    stack[0] = 90; float last = 8.25f; memcpy(stack + 1, &last, 4);
    call("mixed", &cpu); assert(cpu.x[0] == 0x1122334455667788ull);
    cpu.x[0] = 0xdeadbeeffedcba98ull; cpu.x[1] = (uint32_t)-3;
    call("small", &cpu); assert(cpu.x[0] == 0xfedcba95u);
    cpu.x[0] = 0xfedcba9876543210ull; call("handle64", &cpu); assert(cpu.x[0] == 0xfedcba9876543210ull);
    uint32_t value = 0; cpu.x[0] = (uintptr_t)&value; cpu.x[1] = (uint32_t)-7;
    call("pointer", &cpu); assert(cpu.x[0] == (uintptr_t)&value && value == (uint32_t)-7);
    cpu.x[0] = (uint32_t)-2; call("enum_value", &cpu); assert(cpu.x[0] == (uint32_t)-2);
    cpu.x[0] = (uintptr_t)marker; call("callback", &cpu); assert(cpu.x[0] == (uintptr_t)marker);
    assert(invoked == 8);
    return 0;
}
