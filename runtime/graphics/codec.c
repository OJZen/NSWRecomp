// Software Opus forwarding adapted from Zelda-Mobile host/codec.c (Apache-2.0).
#include "dispatch.h"
#include "host_compat.h"
uint64_t graphics_guest_export(const char *name);

void hle__ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
void hle__ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm(Cpu *c) {
    c->pc = graphics_guest_export("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    if (!rt_find_function(c->pc)) die("Missing compiled software Opus helper");
    rt_dispatch(c);
}
