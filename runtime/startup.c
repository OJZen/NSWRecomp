// Bounded SDK initialization probe. Guest code is always a compiled C function.
#include "dispatch.h"
#include "load.h"
#include "kernel.h"
#include "services.h"
#include "threads.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/ucontext.h>

static const char *stages[] = {"_init_libc0", "nnosInitialize", "_init_libc1",
    "nninitInitializeSdkModule", "nninitStartup", "_init_libc2", "nnMain"};
static _Atomic(const char *) stage = "prepare";
static _Thread_local uint64_t last_function, last_lr;
static atomic_uint_fast64_t calls;
static atomic_uint completed_stages;
static atomic_bool constructors_completed, entered_main;
static Cpu cpu;

static void report(void) {
    printf("{\"completed_stages\":%u,\"last_stage\":\"%s\",\"function_calls\":%llu,"
           "\"constructors_completed\":%s,\"entered_nnMain\":%s,"
           "\"initialization_executed\":%s,\"title_verified\":false}\n",
           atomic_load(&completed_stages), atomic_load(&stage), (unsigned long long)atomic_load(&calls),
           atomic_load(&constructors_completed) ? "true" : "false", atomic_load(&entered_main) ? "true" : "false",
           atomic_load(&calls) ? "true" : "false");
}

void fnprof_hit(uint64_t address, uint64_t lr, uint64_t x0) {
    (void)x0;
    last_function = address;
    last_lr = lr;
    calls++;
    threads_checkpoint();
}

static void fault(int signal, siginfo_t *info, void *context) {
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = context;
    if ((signal == SIGSEGV || signal == SIGBUS) && uc &&
        kernel_handle_fault(info->si_addr, (uc->uc_mcontext->__es.__esr >> 6) & 1, context)) return;
#else
    (void)context;
#endif
    dprintf(STDERR_FILENO, "STARTUP fault stage=%s signal=%d address=%p last_entered=%llx lr=%llx\n",
            stage, signal, info->si_addr, (unsigned long long)last_function, (unsigned long long)last_lr);
    _exit(128 + signal);
}

static void guest_trace(Cpu *c) {
    uint64_t frame = c->x[29];
    for (unsigned i = 0; i < 32 && frame && !(frame & 7) && kernel_access(frame, 16, 1); i++) {
        uint64_t next = LD64(frame), lr = LD64(frame + 8);
        fprintf(stderr, "STARTUP caller lr=%llx\n", (unsigned long long)lr);
        if (next <= frame || next - frame > 0x100000) break;
        frame = next;
    }
}

void rt_report_missing(Cpu *c) {
    fprintf(stderr, "STARTUP dispatch miss stage=%s last_entered=%llx lr=%llx\n", stage,
            (unsigned long long)last_function, (unsigned long long)last_lr);
    guest_trace(c);
}

void rt_svc(Cpu *c, uint32_t number, uint64_t pc) {
    threads_checkpoint();
    if (kernel_svc(number, c->x)) { threads_checkpoint(); return; }
    if (number == 0x16) { c->x[0] = service_close((uint32_t)c->x[0]) ? 0 : 0xe401; return; }
    if (number == 0x1f) {
        char name[12] = {0};
        for (size_t i = 0; i < sizeof name; i++) {
            if (c->x[1] > UINT64_MAX - i || !kernel_access(c->x[1] + i, 1, 1)) { c->x[0] = 0xe601; return; }
            name[i] = (char)LD8(c->x[1] + i);
            if (!name[i]) break;
        }
        if (!memchr(name, 0, sizeof name)) { c->x[0] = 0xe601; return; }
        uint32_t handle;
        if (service_connect(name, &handle)) { c->x[0] = 0; c->x[1] = handle; return; }
        fprintf(stderr, "STARTUP service stopped: %s\n", service_blocker());
    }
    if (number == 0x21) {
        uint32_t result;
        if (service_request((uint32_t)c->x[0], (void *)(uintptr_t)c->tpidrro, 0x100, &result)) {
            c->x[0] = result;
            return;
        }
        fprintf(stderr, "STARTUP IPC stopped: %s\n", service_blocker());
    }
    if (number == 0x26 && c->x[2] == 4 && kernel_access(c->x[1], 4, 1))
        fprintf(stderr, "STARTUP guest break result=%x\n", LD32(c->x[1]));
    guest_trace(c);
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

#ifdef TOTK_GRAPHICS
int startup_run(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
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
    unsigned timeout = 30;
#ifdef TOTK_GRAPHICS
    uint64_t requested;
    if (!getenv("TOTK_RUN_SECONDS") || !number(getenv("TOTK_RUN_SECONDS"), &requested) || requested < 1 || requested > 600)
        return LOAD_INVALID;
    timeout = (unsigned)requested;
#endif
    alarm(timeout);
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
    if (atexit(report)) return LOAD_INVALID;
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
            atomic_store(&constructors_completed, true);
        }
        stage = stages[i];
        if (i == 6) atomic_store(&entered_main, true);
        fprintf(stderr, "STARTUP enter %s\n", stage);
        cpu.x[0] = i == 1 ? main_handle : 0;
        cpu.x[1] = 0;
        call(entries[i]);
        atomic_store(&completed_stages, i + 1);
        fprintf(stderr, "STARTUP return %s\n", stage);
    }
    if (!threads_stop()) return 0; // Process exit reclaims a thread that did not reach a safe point.
    service_shutdown();
    munmap(stack, region_size);
    kernel_destroy();
    unload_image(&image);
    return 0;
}
