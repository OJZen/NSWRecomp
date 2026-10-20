// Bounded SDK initialization probe. Guest code is always a compiled C function.
#include "dispatch.h"
#include "load.h"
#include "kernel.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static const char *stages[] = {"_init_libc0", "nnosInitialize", "_init_libc1",
    "nninitInitializeSdkModule", "nninitStartup", "_init_libc2", "nnMain"};
static const char *stage = "prepare";
static uint64_t last_function, last_lr, calls;
static Cpu cpu;

void fnprof_hit(uint64_t address, uint64_t lr, uint64_t x0) {
    (void)x0;
    last_function = address;
    last_lr = lr;
    calls++;
}

static void fault(int signal, siginfo_t *info, void *context) {
    (void)context;
    dprintf(STDERR_FILENO, "STARTUP fault stage=%s signal=%d address=%p function=%llx lr=%llx\n",
            stage, signal, info->si_addr, (unsigned long long)last_function, (unsigned long long)last_lr);
    _exit(128 + signal);
}

void rt_svc(Cpu *c, uint32_t number, uint64_t pc) {
    if (kernel_svc(number, c->x)) return;
    fprintf(stderr, "STARTUP unsupported svc=%x stage=%s pc=%llx x0=%llx x1=%llx x2=%llx x3=%llx\n",
            number, stage, (unsigned long long)pc, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    exit(78);
}

void rt_brk(Cpu *c, uint32_t immediate, uint64_t pc) {
    (void)c;
    fprintf(stderr, "STARTUP break stage=%s pc=%llx immediate=%x\n", stage, (unsigned long long)pc, immediate);
    exit(78);
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    fprintf(stderr, "STARTUP unsupported instruction stage=%s pc=%llx\n", stage, (unsigned long long)pc);
    exit(78);
}

uint64_t rt_ticks(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (uint64_t)time.tv_sec * 19200000 + (uint64_t)time.tv_nsec * 12 / 625;
}

static int has_function(uint64_t address) { return rt_find_function(address) != NULL; }

static void call(uint64_t address) {
    uint64_t saved_lr = cpu.x[30], saved_sp = cpu.sp;
    cpu.x[30] = 0;
    cpu.pc = address;
    rt_dispatch(&cpu);
    if (cpu.sp != saved_sp) {
        fprintf(stderr, "STARTUP unbalanced guest stack stage=%s\n", stage);
        exit(78);
    }
    cpu.x[30] = saved_lr;
}

static int number(const char *text, uint64_t *value) {
    char *end;
    errno = 0;
    *value = strtoull(text, &end, 0);
    return *text && *text != '-' && !*end && !errno;
}

int main(int argc, char **argv) {
    uint64_t through, entries[7];
    if (argc != 10 || !number(argv[2], &through) || !through || through > 7) return LOAD_INVALID;
    for (unsigned i = 0; i < 7; i++)
        if (!number(argv[i + 3], &entries[i]) || !has_function(entries[i])) return LOAD_INVALID;
    if (rt_validate_tables()) return LOAD_INVALID;
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core)) return LOAD_INVALID;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGALRM};
    for (unsigned i = 0; i < sizeof signals / sizeof *signals; i++)
        if (sigaction(signals[i], &action, NULL)) return LOAD_INVALID;
    alarm(30);
    LoadedImage image = {0};
    int result = load_plan(argv[1], &image, has_function);
    if (result) return result;
    size_t page = image.page_size, stack_size = 8 << 20, region_size = stack_size + 2 * page;
    unsigned char *stack = mmap(NULL, region_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) { unload_image(&image); return LOAD_INVALID; }
    if (mprotect(stack + page, stack_size, PROT_READ | PROT_WRITE)) {
        munmap(stack, region_size); unload_image(&image); return LOAD_INVALID;
    }
    _Alignas(16) unsigned char tls[8192] = {0}, tlr[0x200] = {0}, thread[0x400] = {0};
    *(uintptr_t *)(tlr + 0x1f8) = (uintptr_t)thread;
    cpu.tpidr = (uintptr_t)tls;
    cpu.tpidrro = (uintptr_t)tlr;
    cpu.sp = (uintptr_t)stack + page + stack_size;
    cpu.excl_addr = UINT64_MAX;
    uint32_t main_handle;
    result = kernel_init(&image, (uintptr_t)stack + page, stack_size, &main_handle);
    if (result) { munmap(stack, region_size); unload_image(&image); return result; }
    g_fnprof = 1;
    for (unsigned i = 0; i < through; i++) {
        if (i == 6) {
            stage = "module constructors";
            // The host already relocated the image; never re-enter the guest rtld.
            for (size_t module = 1; module < image.count; module++) {
                LoadHeader *h = &image.modules[module].header;
                if (h->init) call(h->base + h->init);
                else for (uint64_t at = 0; at < h->init_array_size; at += 8) {
                    uint64_t function = LD64(h->base + h->init_array + at);
                    if (function && function != UINT64_MAX) call(function);
                }
            }
        }
        stage = stages[i];
        fprintf(stderr, "STARTUP enter %s\n", stage);
        cpu.x[0] = i == 1 ? main_handle : 0;
        cpu.x[1] = 0;
        call(entries[i]);
        fprintf(stderr, "STARTUP return %s\n", stage);
    }
    printf("{\"completed_stages\":%llu,\"last_stage\":\"%s\",\"function_calls\":%llu,"
           "\"initialization_executed\":true,\"title_verified\":false}\n",
           (unsigned long long)through, stage, (unsigned long long)calls);
    munmap(stack, region_size);
    kernel_destroy();
    unload_image(&image);
    return 0;
}
