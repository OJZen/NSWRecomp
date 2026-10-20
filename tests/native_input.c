#include "dispatch.h"
#include "host_compat.h"
#include "input.h"
#include <assert.h>
#define HLE(name) void hle_##name(Cpu *);
#include "hle_symbols.inc"
#undef HLE

static _Alignas(16) uint64_t memory_words[32];
void die(const char *message, ...) { (void)message; exit(78); }
bool kernel_access(uint64_t address, size_t size, unsigned permission) {
    (void)permission;
    return address >= (uintptr_t)memory_words && size <= sizeof memory_words &&
           address - (uintptr_t)memory_words <= sizeof memory_words - size;
}
void dm_guest_write_prepare(void *address, size_t size) { assert(kernel_access((uintptr_t)address, size, 2)); }

static void sample(uint32_t id) {
    memory_words[16] = id;
    Cpu c = {0}; c.x[0] = (uintptr_t)memory_words; c.x[1] = 2; c.x[2] = (uintptr_t)(memory_words + 16);
    hle__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(&c); assert(c.x[0] == 1);
}
int main(int argc, char **argv) {
    (void)argv;
    Cpu c = {0};
    if (argc > 1) { c.x[0] = 0; c.x[1] = 1; hle__ZN2nn3hid22SetSupportedNpadIdTypeEPKjm(&c); return 99; }
    memory_words[5] = 0xfeedface;
    hle__ZN2nn3hid14InitializeNpadEv(&c);
    sample(0); assert(memory_words[0] && !memory_words[1] && memory_words[4] == 3);
    uint64_t first = memory_words[0];
    input_key(7, 1); input_key(7, 0); sample(0);
    assert(memory_words[0] > first && memory_words[1] == 1);
    sample(0); assert(!memory_words[1]);
    input_key(2, 1); input_key(13, 1); sample(0);
    assert((int32_t)memory_words[2] == 32767 && (int32_t)(memory_words[2] >> 32) == 32767);
    input_clear(); sample(0); assert(!memory_words[1] && !memory_words[2]);
    sample(2); assert(!memory_words[4]);
    c.x[0] = 2; hle__ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(&c);
    sample(0); assert(!memory_words[4]);
    c.x[0] = (uintptr_t)(memory_words + 16); hle__ZN2nn3hid15GetNpadStyleSetERKj(&c); assert(!c.x[0]);
    assert(memory_words[5] == 0xfeedface);
    return 0;
}
