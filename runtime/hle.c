#include "dispatch.h"
#include <stdio.h>
#include <stdlib.h>

// Link-only probes can resolve these names; execution requires a real backend.
__attribute__((weak)) void rt_report_missing_host_api(Cpu *c, const char *name) { (void)c; (void)name; }
#define HLE(name) __attribute__((weak)) void hle_##name(Cpu *c) { \
    rt_report_missing_host_api(c, #name); \
    fprintf(stderr, "missing host API: %s lr=%llx\n", #name, (unsigned long long)c->x[30]); \
    exit(78); \
}
#include "hle_symbols.inc"
#ifdef _WIN32
#include "hle_windows_symbols.inc"
#endif
#undef HLE
