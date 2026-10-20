#include "dispatch.h"
#include <stdio.h>
#include <stdlib.h>

// Link-only probes can resolve these names; execution requires a real backend.
#define HLE(name) __attribute__((weak)) void hle_##name(Cpu *c) { \
    fprintf(stderr, "missing host API: %s lr=%llx\n", #name, (unsigned long long)c->x[30]); \
    exit(78); \
}
#include "hle_symbols.inc"
#undef HLE
