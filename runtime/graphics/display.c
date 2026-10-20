#include "dispatch.h"
#include "host_compat.h"

uint32_t front_vsync_handle(void);
uint64_t graphics_guest_export(const char *name);
static uint32_t display, layer, native;

static bool output(Cpu *c, void *object) {
    if (!kernel_access(c->x[0], 8, 2)) { c->x[0] = 0x202; return false; }
    dm_guest_write_prepare((void *)(uintptr_t)c->x[0], 8);
    ST64(c->x[0], (uintptr_t)object); c->x[0] = 0; return true;
}
void hle__ZN2nn2vi10InitializeEv(Cpu *c) {
    (void)c;
    if (!front_metal_layer()) die("Display initialization requires a native Metal window");
}
void hle__ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE(Cpu *c) { output(c, &display); }
void hle__ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE(Cpu *c) {
    if (c->x[1] != (uintptr_t)&display) { c->x[0] = 0x202; return; }
    output(c, &layer);
}
void hle__ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(Cpu *c) {
    if (c->x[1] != (uintptr_t)&layer) { c->x[0] = 0x202; return; }
    output(c, &native);
}
void hle__ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE(Cpu *c) {
    if (c->x[1] != (uintptr_t)&display || !kernel_access(c->x[0], 0x40, 3)) { c->x[0] = 0x202; return; }
    uint32_t handle = front_vsync_handle();
    if (!handle) { c->x[0] = 0xce01; return; }
    Cpu attach = *c;
    attach.pc = graphics_guest_export("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    if (!rt_find_function(attach.pc)) die("Missing compiled SystemEvent attachment helper");
    attach.x[1] = handle; attach.x[2] = 1; attach.x[3] = 1; attach.x[30] = 0; attach.excl_addr = UINT64_MAX;
    rt_dispatch(&attach);
    if (attach.sp != c->sp) die("Unbalanced SystemEvent attachment stack");
    c->fpsr = attach.fpsr; c->x[0] = 0;
}
