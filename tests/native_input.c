#include "dispatch.h"
#include "host_compat.h"
#include "input.h"
#include <assert.h>
#include <math.h>
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
    Cpu c = {0};
    if (argc > 1) {
        if (!strcmp(argv[1], "bad-single-id")) {
            c.x[0] = (uintptr_t)memory_words; c.x[1] = (uintptr_t)memory_words + sizeof memory_words - 3;
            hle__ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj(&c);
        } else if (!strcmp(argv[1], "bad-single-output")) {
            c.x[0] = (uintptr_t)memory_words + sizeof memory_words - 39; c.x[1] = (uintptr_t)(memory_words + 16);
            hle__ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj(&c);
        } else { c.x[0] = 0; c.x[1] = 1; hle__ZN2nn3hid22SetSupportedNpadIdTypeEPKjm(&c); }
        return 99;
    }
    memory_words[5] = 0xfeedface;
    hle__ZN2nn3hid14InitializeNpadEv(&c);
    sample(0); assert(memory_words[0] && !memory_words[1] && memory_words[4] == 3);
    void (*single[])(Cpu *) = {
        hle__ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj,
        hle__ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj,
        hle__ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj,
        hle__ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj,
        hle__ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj,
    };
    for (unsigned i = 0; i < sizeof single / sizeof *single; i++) {
        uint64_t previous = memory_words[0];
        input_key(7, 1); input_key(7, 0);
        c.x[0] = (uintptr_t)memory_words; c.x[1] = (uintptr_t)(memory_words + 16); c.x[2] = 0;
        c.x[19] = 0x12345678;
        single[i](&c);
        assert(memory_words[0] > previous && memory_words[1] == (i == 0));
        assert(memory_words[4] == (i == 0 ? 3 : 0) && memory_words[5] == 0xfeedface);
        assert(c.x[19] == 0x12345678);
        input_clear();
    }
    memory_words[16] = 2; c.x[0] = (uintptr_t)memory_words; c.x[1] = (uintptr_t)(memory_words + 16);
    single[0](&c); assert(!memory_words[4]); sample(0);
    uint64_t first = memory_words[0];
    input_key(7, 1); input_key(7, 0); sample(0);
    assert(memory_words[0] > first && memory_words[1] == 1);
    sample(0); assert(!memory_words[1]);
    input_key(2, 1); input_key(13, 1); sample(0);
    assert((int32_t)memory_words[2] == 32767 && (int32_t)(memory_words[2] >> 32) == 32767);
    input_clear(); sample(0); assert(!memory_words[1] && !memory_words[2]);
    input_controller(1, .1f, -.1f, 1, -1); input_controller(0, .1f, -.1f, 1, -1);
    sample(0); assert(memory_words[1] == 1 && !memory_words[2]);
    int32_t *axes = (int32_t *)(memory_words + 2);
    assert(axes[2] > 23000 && axes[2] < 23200 && axes[3] == -axes[2]);
    sample(0); assert(!memory_words[1]);
    input_controller(2, 1, 0, NAN, INFINITY); input_key(7, 1); input_key(13, 1);
    sample(0); assert(memory_words[1] == 3 && axes[0] == 0 && axes[1] == 32767 && !memory_words[3]);
    input_controller_clear(); sample(0); assert(memory_words[1] == 1 && axes[1] == 32767);
    input_clear(); input_controller(0xffff, -2, 0, 0, 2); sample(0);
    assert(memory_words[1] == 0xffff && axes[0] == -32767 && axes[3] == 32767);
    input_clear(); sample(0); assert(!memory_words[1] && !memory_words[2] && !memory_words[3]);
    sample(2); assert(!memory_words[4]);
    c.x[0] = 2; hle__ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(&c);
    sample(0); assert(!memory_words[4]);
    c.x[0] = (uintptr_t)(memory_words + 16); hle__ZN2nn3hid15GetNpadStyleSetERKj(&c); assert(!c.x[0]);
    assert(memory_words[5] == 0xfeedface);
    return 0;
}
