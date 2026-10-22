#include "dispatch.h"
#include "nvn_bridge.h"
#include "host_compat.h"
#include "nvn_mtl.h"
#include "movie.h"

// Tokens occupy no executable host pages and cannot alias the mapped guest modules.
#define NVN_BASE 0x7f00000000ull

uint64_t nvn_guest_address(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < nvn_bridge_count; i++)
        if (!strcmp(name, nvn_bridge_entries[i].name)) return NVN_BASE + 4 * i;
    fprintf(stderr, "unknown NVN API lookup: %.128s\n", name);
    return 0;
}

GuestFn rt_find_external(uint64_t address) {
    uint64_t offset = address - NVN_BASE;
    return !(offset & 3) && offset / 4 < nvn_bridge_count ? nvn_bridge_entries[offset / 4].call : movie_find_function(address);
}

void *nvn_bridge_native(size_t index) {
    return index < nvn_bridge_count ? nvn_backend_native(index) : NULL;
}

void nvn_bridge_unavailable(Cpu *c, const char *name) {
    fprintf(stderr, "unsupported NVN API: %s lr=%llx\n", name, (unsigned long long)c->x[30]);
    exit(78);
}

uint64_t nvn_bridge_stack(Cpu *c, size_t offset) {
    if (offset > UINT64_MAX - c->sp || !kernel_access(c->sp + offset, 8, 1))
        die("NVN stack argument outside guest memory");
    return LD64(c->sp + offset);
}

void nvn_guest_callback(uint64_t address, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!nvn_bridge_cpu || !rt_find_function(address)) die("Invalid NVN guest callback");
    Cpu callback = *nvn_bridge_cpu;
    callback.pc = address; callback.x[30] = 0;
    callback.excl_addr = UINT64_MAX;
    callback.x[0] = a0; callback.x[1] = a1; callback.x[2] = a2; callback.x[3] = a3;
    rt_dispatch(&callback);
    if (callback.sp != nvn_bridge_cpu->sp) die("Unbalanced NVN callback stack");
    nvn_bridge_cpu->fpsr = callback.fpsr;
}

static uint64_t allocators[2][4];
void hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *c) {
    memcpy(allocators[0], c->x, sizeof allocators[0]);
}
void hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *c) {
    memcpy(allocators[1], c->x, sizeof allocators[1]);
}
void hle__ZN2nv18InitializeGraphicsEPvm(Cpu *c) {
    if (!kernel_access(c->x[0], c->x[1], 3)) die("Invalid NV graphics transfer memory");
    mtl_init();
}
void hle__ZN2nv16FinalizeGraphicsEv(Cpu *c) { (void)c; mtl_finish(); }
void hle_nvnBootstrapLoader(Cpu *c) {
    char name[129];
    for (size_t i = 0; i < sizeof name; i++) {
        if (c->x[0] > UINT64_MAX - i || !kernel_access(c->x[0] + i, 1, 1)) die("Invalid NVN API name");
        name[i] = (char)LD8(c->x[0] + i);
        if (!name[i]) { c->x[0] = nvn_guest_address(name); return; }
    }
    die("NVN API name is too long");
}
