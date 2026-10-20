// Isolated function validation only; unsupported runtime work must stop the probe.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

extern const uint64_t rtld_fn_addrs[], rtld_fn_count;
extern const GuestFn rtld_fn_ptrs[];
#ifdef TOTK_LINK_ALL
extern const uint64_t main_fn_addrs[], main_fn_count, subsdk0_fn_addrs[], subsdk0_fn_count, sdk_fn_addrs[], sdk_fn_count;
extern const GuestFn main_fn_ptrs[], subsdk0_fn_ptrs[], sdk_fn_ptrs[];
#endif
static const struct { const char *name; const uint64_t *addresses, *count; const GuestFn *functions; } tables[] = {
    {"rtld", rtld_fn_addrs, &rtld_fn_count, rtld_fn_ptrs},
#ifdef TOTK_LINK_ALL
    {"main", main_fn_addrs, &main_fn_count, main_fn_ptrs},
    {"subsdk0", subsdk0_fn_addrs, &subsdk0_fn_count, subsdk0_fn_ptrs},
    {"sdk", sdk_fn_addrs, &sdk_fn_count, sdk_fn_ptrs},
#endif
};
volatile int g_fnprof = 0;

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
static void stop(void) __attribute__((noreturn));
static void stop(void) { fputs("probe reached unsupported runtime work\n", stderr); abort(); }
void rt_dispatch(Cpu *c) {
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++) {
        uint64_t lo = 0, hi = *tables[t].count;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (tables[t].addresses[mid] < c->pc) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *tables[t].count && tables[t].addresses[lo] == c->pc) {
            tables[t].functions[lo](c);
            return;
        }
    }
    stop();
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; stop(); }
uint64_t rt_ticks(void) { stop(); }
size_t probe_cpu_size(void) { return sizeof(Cpu); }

#ifdef TOTK_LINK_ALL
int main(void) {
    uint64_t previous = 0;
    fputs("{\"modules\":{", stdout);
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++) {
        if (!*tables[t].count) return 1;
        for (uint64_t i = 0; i < *tables[t].count; i++) {
            if (!tables[t].functions[i] || tables[t].addresses[i] <= previous) return 2;
            previous = tables[t].addresses[i];
        }
        printf("%s\"%s\":%llu", t ? "," : "", tables[t].name, (unsigned long long)*tables[t].count);
    }
    puts("},\"game_executed\":false}");
    return 0;
}
#endif
