// Isolated function validation only; unsupported runtime work must stop the probe.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

extern const uint64_t rtld_fn_addrs[], rtld_fn_count;
extern const GuestFn rtld_fn_ptrs[];
volatile int g_fnprof = 0;

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
static void stop(void) __attribute__((noreturn));
static void stop(void) { fputs("probe reached unsupported runtime work\n", stderr); abort(); }
void rt_dispatch(Cpu *c) {
    uint64_t lo = 0, hi = rtld_fn_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (rtld_fn_addrs[mid] < c->pc) lo = mid + 1;
        else hi = mid;
    }
    if (lo == rtld_fn_count || rtld_fn_addrs[lo] != c->pc) stop();
    rtld_fn_ptrs[lo](c);
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; stop(); }
uint64_t rt_ticks(void) { stop(); }
size_t probe_cpu_size(void) { return sizeof(Cpu); }
