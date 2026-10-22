// Isolated function validation only; unsupported runtime work must stop the probe.
#include "dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TOTK_LINK_ALL
#include "load.h"
#endif

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

#ifndef TOTK_STARTUP
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
#endif
static void stop(void) __attribute__((noreturn));
static void stop(void) { fputs("probe reached unsupported runtime work\n", stderr); exit(78); }
__attribute__((weak)) GuestFn rt_find_external(uint64_t address) { (void)address; return NULL; }
__attribute__((weak)) void rt_report_missing(Cpu *cpu) { (void)cpu; }
// Adapted from LASRecomp runtime/dispatch.c (MIT); only immutable compiled functions enter this cache.
enum { DISPATCH_CACHE_SIZE = 4096 };
static _Thread_local struct { uint64_t address; GuestFn function; } dispatch_cache[DISPATCH_CACHE_SIZE];
GuestFn rt_find_function(uint64_t address) {
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++) {
        uint64_t lo = 0, hi = *tables[t].count;
        if (!hi || address < tables[t].addresses[0] || address > tables[t].addresses[hi - 1]) continue;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (tables[t].addresses[mid] < address) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *tables[t].count && tables[t].addresses[lo] == address) {
            unsigned slot = (unsigned)(address >> 2) & (DISPATCH_CACHE_SIZE - 1);
            dispatch_cache[slot].address = address;
            return dispatch_cache[slot].function = tables[t].functions[lo];
        }
    }
    return rt_find_external(address);
}
__attribute__((noinline)) static void dispatch_miss(Cpu *c) {
    GuestFn function = rt_find_function(c->pc);
    if (!function) {
        rt_report_missing(c);
        fprintf(stderr, "missing compiled function: pc=%llx lr=%llx\n",
                (unsigned long long)c->pc, (unsigned long long)c->x[30]);
        stop();
    }
    __attribute__((musttail)) return function(c);
}
void rt_dispatch(Cpu *c) {
    unsigned slot = (unsigned)(c->pc >> 2) & (DISPATCH_CACHE_SIZE - 1);
    GuestFn function = dispatch_cache[slot].function;
    if (__builtin_expect(function && dispatch_cache[slot].address == c->pc, 1))
        __attribute__((musttail)) return function(c);
    __attribute__((musttail)) return dispatch_miss(c);
}
#ifndef TOTK_STARTUP
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; stop(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; stop(); }
uint64_t rt_ticks(void) { stop(); }
#endif
size_t probe_cpu_size(void) { return sizeof(Cpu); }

int rt_validate_tables(void) {
    uint64_t previous = 0;
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++) {
        if (!*tables[t].count) return 1;
        for (uint64_t i = 0; i < *tables[t].count; i++) {
            if (!tables[t].functions[i] || tables[t].addresses[i] <= previous) return 2;
            previous = tables[t].addresses[i];
        }
    }
    return 0;
}

#if defined(TOTK_LINK_ALL) && !defined(TOTK_STARTUP)
static int has_function(uint64_t address) { return rt_find_function(address) != NULL; }
int main(int argc, char **argv) {
    int valid = rt_validate_tables();
    if (valid) return valid;
    if (argc == 3 && !strcmp(argv[1], "--load-only")) {
        LoadedImage image = {0};
        int result = load_plan(argv[2], &image, has_function);
        if (result) return result;
        if (image.count != sizeof tables / sizeof *tables) { unload_image(&image); return 1; }
        printf("{\"modules\":%zu,\"mapped_bytes\":%llu,\"host_page_bytes\":%zu,"
               "\"mixed_permission_pages\":%zu,\"relocations\":%llu,\"init_functions\":%llu,"
               "\"entry_functions\":%llu,\"game_executed\":false}\n", image.count,
               (unsigned long long)image.bytes, image.page_size, image.mixed_pages,
               (unsigned long long)image.relocations, (unsigned long long)image.init_functions,
               (unsigned long long)image.entry_functions);
        unload_image(&image);
        return 0;
    }
    if (argc != 1) return 1;
    fputs("{\"modules\":{", stdout);
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++)
        printf("%s\"%s\":%llu", t ? "," : "", tables[t].name, (unsigned long long)*tables[t].count);
    puts("},\"game_executed\":false}");
    return 0;
}
#endif
