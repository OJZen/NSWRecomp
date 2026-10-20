#include "dispatch.h"
#include "nvn_bridge.h"
#include "host_compat.h"
#include <assert.h>

uint64_t nvn_guest_address(const char *);
void hle_nvnBootstrapLoader(Cpu *);
static unsigned calls;
static int unbalanced;
static unsigned char storage[256];
_Thread_local Cpu *nvn_bridge_cpu;
static void synthetic(Cpu *c) { c->x[0] += 3; }
const NvnBridgeEntry nvn_bridge_entries[] = {{"nvnSynthetic", synthetic}};
const size_t nvn_bridge_count = 1;

void die(const char *format, ...) { (void)format; exit(78); }
void mtl_init(void) { abort(); }
void mtl_finish(void) { abort(); }
void *nvn_backend_native(const char *name) { assert(!strcmp(name, "nvnSynthetic")); return (void *)synthetic; }
bool kernel_access(uint64_t address, size_t n, unsigned permissions) {
    return permissions == 1 && address >= (uintptr_t)storage && n <= sizeof storage && address - (uintptr_t)storage <= sizeof storage - n;
}
static void callback(Cpu *c) {
    assert(c != nvn_bridge_cpu && c->sp == nvn_bridge_cpu->sp && !c->x[30] && c->excl_addr == UINT64_MAX);
    assert(c->x[0] == 10 && c->x[1] == 11 && c->x[2] == 12 && c->x[3] == 13);
    c->x[19] = 99; c->fpsr |= 1;
    c->sp += unbalanced ? 8 : 0;
    calls++;
}
GuestFn rt_find_function(uint64_t address) { return address == 0x1234 ? callback : rt_find_external(address); }
void rt_dispatch(Cpu *c) { GuestFn fn = rt_find_function(c->pc); assert(fn); fn(c); }

int main(int argc, char **argv) {
    (void)argv;
    unbalanced = argc > 1;
    Cpu c = {0}; c.sp = (uintptr_t)storage; c.x[19] = 7; c.excl_addr = 5;
    uint64_t token = nvn_guest_address("nvnSynthetic");
    assert(token && rt_find_external(token) == synthetic);
    assert(!rt_find_external(token + 1) && !rt_find_external(token - 4) && !rt_find_external(token + 4));
    assert(!nvn_guest_address(NULL) && !nvn_guest_address("nvnUnknown"));
    strcpy((char *)storage, "nvnSynthetic"); c.x[0] = (uintptr_t)storage;
    hle_nvnBootstrapLoader(&c); assert(c.x[0] == token);
    c.x[0] = 9; c.pc = token; rt_dispatch(&c); assert(c.x[0] == 12);
    assert(nvn_bridge_native(0) == (void *)synthetic && !nvn_bridge_native(1));
    nvn_bridge_cpu = &c;
    nvn_guest_callback(0x1234, 10, 11, 12, 13);
    assert(calls == 1 && c.x[0] == 12 && c.x[19] == 7 && c.excl_addr == 5 && c.fpsr == 1);
    assert(nvn_bridge_cpu == &c);
    return 0;
}
