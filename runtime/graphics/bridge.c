#include "dispatch.h"
#include "nvn_bridge.h"
#include "host_compat.h"
#ifndef NSW_VULKAN
#include "nvn_mtl.h"
#endif
#if !defined(NSW_VULKAN) || defined(NSW_VULKAN_VIDEO)
#include "movie.h"
#endif
#include "guest_callback.h"

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
    if (!(offset & 3) && offset / 4 < nvn_bridge_count) return nvn_bridge_entries[offset / 4].call;
#if defined(NSW_VULKAN) && !defined(NSW_VULKAN_VIDEO)
    return NULL;
#else
    return movie_find_function(address);
#endif
}

void *nvn_bridge_native(size_t index) {
    return index < nvn_bridge_count ? nvn_backend_native(index) : NULL;
}

void nvn_bridge_unavailable(Cpu *c, const char *name) {
    fprintf(stderr, "unsupported NVN API: %s lr=%llx\n", name, (unsigned long long)c->x[30]);
#ifdef NSW_VULKAN
    extern void rt_report_graphics_error(Cpu *, const char *);
    rt_report_graphics_error(c, name);
#endif
    exit(78);
}

uint64_t nvn_bridge_stack(Cpu *c, size_t offset) {
    if (offset > UINT64_MAX - c->sp || !kernel_access(c->sp + offset, 8, 1))
        die("NVN stack argument outside guest memory");
    return LD64(c->sp + offset);
}

void nvn_guest_callback(uint64_t address, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    const uint64_t arguments[] = {a0, a1, a2, a3};
    if (!guest_callback(nvn_bridge_cpu, address, arguments, 4, NULL))
        die("Invalid NVN guest callback or unbalanced callback stack");
}
#ifndef NSW_VULKAN
void hle__ZN2nv18InitializeGraphicsEPvm(Cpu *c) {
    if (!kernel_access(c->x[0], c->x[1], 3)) die("Invalid NV graphics transfer memory");
    mtl_init();
}
void hle__ZN2nv16FinalizeGraphicsEv(Cpu *c) { (void)c; mtl_finish(); }
#endif
void hle_nvnBootstrapLoader(Cpu *c) {
    char name[129];
    for (size_t i = 0; i < sizeof name; i++) {
        if (c->x[0] > UINT64_MAX - i || !kernel_access(c->x[0] + i, 1, 1)) die("Invalid NVN API name");
        name[i] = (char)LD8(c->x[0] + i);
        if (!name[i]) { c->x[0] = nvn_guest_address(name); return; }
    }
    die("NVN API name is too long");
}
