// Keyboard-backed FullKey state, adapted from Zelda-Mobile host/input.c (Apache-2.0).
#include "input.h"
#include "dispatch.h"
#include "host_compat.h"

typedef struct { uint64_t sampling, buttons; int32_t lx, ly, rx, ry; uint32_t attributes, reserved; } PadState;
_Static_assert(sizeof(PadState) == 0x28, "Npad state ABI");
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];
static uint64_t sampling, latched;
static uint32_t styles = 1, enabled = 1;
static const struct { unsigned key, bit; } mapping[] = {
    {7,0}, {6,1}, {15,2}, {3,3}, {8,4}, {9,5}, {12,6}, {14,7},
    {56,8}, {49,9}, {36,10}, {48,11}, {123,12}, {126,13}, {124,14}, {125,15},
};
void input_key(unsigned code, int pressed) {
    if (code >= sizeof keys) return;
    pthread_mutex_lock(&lock);
    if (pressed && !keys[code]) for (size_t i = 0; i < sizeof mapping / sizeof *mapping; i++)
        if (mapping[i].key == code) latched |= 1ull << mapping[i].bit;
    keys[code] = !!pressed;
    pthread_mutex_unlock(&lock);
}
void input_clear(void) {
    pthread_mutex_lock(&lock); memset(keys, 0, sizeof keys); latched = 0; pthread_mutex_unlock(&lock);
}
static void memory(uint64_t address, size_t size, unsigned permissions) {
    if (!kernel_access(address, size, permissions)) die("Invalid HID memory");
    if (permissions & 2) dm_guest_write_prepare((void *)(uintptr_t)address, size);
}
void hle__ZN2nn3hid14InitializeNpadEv(Cpu *c) { (void)c; input_clear(); }
void hle__ZN2nn3hid22SetSupportedNpadIdTypeEPKjm(Cpu *c) {
    if (c->x[1] > 10) die("Invalid Npad ID count");
    if (c->x[1]) memory(c->x[0], c->x[1] * 4, 1);
    pthread_mutex_lock(&lock); enabled = 0;
    for (uint64_t i = 0; i < c->x[1]; i++) if (!LD32(c->x[0] + i * 4)) enabled = 1;
    pthread_mutex_unlock(&lock);
}
void hle__ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(Cpu *c) {
    pthread_mutex_lock(&lock); styles = (uint32_t)c->x[0]; pthread_mutex_unlock(&lock);
}
void hle__ZN2nn3hid15GetNpadStyleSetERKj(Cpu *c) {
    memory(c->x[0], 4, 1); uint32_t id = LD32(c->x[0]);
    pthread_mutex_lock(&lock); c->x[0] = !id && enabled ? styles & 1 : 0; pthread_mutex_unlock(&lock);
}
static void states(Cpu *c, unsigned style) {
    if ((int32_t)c->x[1] <= 0) { c->x[0] = 0; return; }
    memory(c->x[0], sizeof(PadState), 2); memory(c->x[2], 4, 1); uint32_t id = LD32(c->x[2]);
    pthread_mutex_lock(&lock);
    PadState state = {.sampling = ++sampling};
    if (!id && enabled && (styles & style & 1)) {
        state.buttons = latched; latched = 0;
        for (size_t i = 0; i < sizeof mapping / sizeof *mapping; i++) if (keys[mapping[i].key]) state.buttons |= 1ull << mapping[i].bit;
        state.lx = (keys[2] - keys[0]) * 32767; state.ly = (keys[13] - keys[1]) * 32767;
        state.rx = (keys[37] - keys[38]) * 32767; state.ry = (keys[34] - keys[40]) * 32767;
        state.attributes = 3;
    }
    memcpy((void *)(uintptr_t)c->x[0], &state, sizeof state);
    pthread_mutex_unlock(&lock); c->x[0] = 1;
}
#define PAD(symbol, style) void hle_##symbol(Cpu *c) { states(c, style); }
PAD(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj, 1)
PAD(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj, 2)
PAD(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj, 4)
PAD(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj, 8)
PAD(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj, 16)
void hle__ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE(Cpu *c) { (void)c; /* FullKey has no detachable halves. */ }
void hle__ZN2nn3hid14DisconnectNpadERKj(Cpu *c) {
    memory(c->x[0], 4, 1); uint32_t id = LD32(c->x[0]);
    pthread_mutex_lock(&lock); if (!id) enabled = 0; pthread_mutex_unlock(&lock);
}
// A keyboard exposes no motion sensor or vibration device.
void hle__ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(Cpu *c) { c->x[0] = 0; }
void hle__ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(Cpu *c) { c->x[0] = 0; }
void hle__ZN2nn3hid15InitializeMouseEv(Cpu *c) { (void)c; }
void hle__ZN2nn3hid18InitializeKeyboardEv(Cpu *c) { (void)c; }
void hle__ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi(Cpu *c) {
    if ((int32_t)c->x[1] <= 0) { c->x[0] = 0; return; }
    memory(c->x[0], 0x28, 2); memset((void *)(uintptr_t)c->x[0], 0, 0x28); c->x[0] = 1;
}
void hle__ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE(Cpu *c) {
    memory(c->x[0], 0x30, 2); memset((void *)(uintptr_t)c->x[0], 0, 0x30);
}
