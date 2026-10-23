// Bounded SDK initialization probe. Guest code is always a compiled C function.
#include "dispatch.h"
#include "load.h"
#include "kernel.h"
#include "host.h"
#ifndef NSW_KERNEL_ONLY
#include "services.h"
#endif
#include "threads.h"
#ifdef NSW_CALLBACK_GUARDS
#include "callback_guard.h"
#endif
#ifdef NSW_VULKAN
#include "vulkan_runtime.h"
#include "vulkan_display.h"
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#include <sys/ucontext.h>
#endif

static const char *stages[] = {"_init_libc0", "nnosInitialize", "_init_libc1",
    "nninitInitializeSdkModule", "nninitStartup", "_init_libc2", "nnMain"};
static _Atomic(const char *) stage = "prepare";
static _Atomic(const char *) stop_reason = "incomplete";
static _Thread_local uint64_t last_function, last_lr;
static atomic_uint_fast64_t calls;
static atomic_uint completed_stages;
static atomic_bool constructors_completed, entered_main;
static atomic_bool window_closed, reported;
static Cpu cpu;
#ifdef NSW_VULKAN
#define RUNTIME_MODE (vulkan_display_window_requested() ? "vulkan_window" : "vulkan_headless")
#elif defined(NSW_KERNEL_ONLY)
#define RUNTIME_MODE "kernel_only"
#elif defined(NSW_BASIC_SERVICES)
#define RUNTIME_MODE "basic_services"
#else
#define RUNTIME_MODE "full"
#endif

static void report(void) {
    if (atomic_exchange(&reported, true)) return;
#ifdef NSW_VULKAN
    HostWindowStats window = vulkan_display_window_stats();
#endif
    printf("{\"completed_stages\":%u,\"last_stage\":\"%s\",\"function_calls\":%llu,"
           "\"constructors_completed\":%s,\"entered_nnMain\":%s,"
           "\"initialization_executed\":%s,\"window_closed\":%s,\"title_verified\":false,"
           "\"stop_reason\":\"%s\",\"runtime_mode\":\"%s\",\"kernel_only\":%s,"
           "\"window_created\":%s,\"window_frames_submitted\":%llu,\"window_frames_painted\":%llu,"
           "\"callback_guards_sha256\":\"%s\",\"callback_guards_completed\":%llu,"
           "\"vulkan_device_created\":%s,\"vulkan_clear_submissions\":%llu,\"vulkan_readback_pixels\":%llu,\"vulkan_depth_clear_submissions\":%llu,\"vulkan_compute_submissions\":%llu,\"vulkan_shader_draws\":%llu}\n",
           atomic_load(&completed_stages), atomic_load(&stage), (unsigned long long)atomic_load(&calls),
           atomic_load(&constructors_completed) ? "true" : "false", atomic_load(&entered_main) ? "true" : "false",
           atomic_load(&calls) ? "true" : "false", atomic_load(&window_closed) ? "true" : "false",
           atomic_load(&stop_reason), RUNTIME_MODE,
#ifdef NSW_KERNEL_ONLY
           "true",
#else
           "false",
#endif
#ifdef NSW_VULKAN
           window.created ? "true" : "false", (unsigned long long)window.submitted, (unsigned long long)window.painted,
#else
           "false", 0ull, 0ull,
#endif
#ifdef NSW_CALLBACK_GUARDS
           callback_guards_sha256, (unsigned long long)callback_guards_completed(),
#else
           "", 0ull,
#endif
#ifdef NSW_VULKAN
           vulkan_runtime_device_created() ? "true" : "false",
           (unsigned long long)vulkan_runtime_clear_count(), (unsigned long long)vulkan_runtime_readback_pixels(),
           (unsigned long long)vulkan_runtime_depth_clear_count(), (unsigned long long)vulkan_runtime_dispatch_count(), (unsigned long long)vulkan_runtime_draw_count()
#else
           "false", 0ull, 0ull, 0ull, 0ull, 0ull
#endif
           );
}

void startup_window_closed(void) {
    atomic_store(&window_closed, true);
    atomic_store(&stop_reason, "window_closed");
    bool stopped = threads_stop();
    report(); // Closing before image initialization must also produce a report.
    if (!stopped) {
        fputs("STARTUP shutdown did not reach a safe point; skipping concurrent service teardown\n", stderr);
        fflush(stdout); _Exit(78);
    }
}

void fnprof_hit(uint64_t address, uint64_t lr, uint64_t x0) {
    threads_trace(address,lr,x0);
    last_function = address;
    last_lr = lr;
    calls++;
    // Closing during image initialization can precede threads_init(), which resets its stop flag.
    if (atomic_load(&window_closed)) threads_request_stop();
    threads_checkpoint();
}

#ifdef _WIN32
static DWORD WINAPI watchdog(void *argument) {
    unsigned seconds=(unsigned)(uintptr_t)argument;
    Sleep(seconds*1000-500);
    fputs("STARTUP native time budget reached; thread snapshot follows\n",stderr);
    threads_dump(); atomic_store(&stop_reason,"time_budget"); report(); fflush(stdout); fflush(stderr);
    _Exit(78);
}
static LONG WINAPI fault(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    void *address = (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2 ? (void *)record->ExceptionInformation[1] : NULL;
    fprintf(stderr, "STARTUP fault stage=%s exception=%lx address=%p host_pc=%p last_entered=%llx lr=%llx\n",
            atomic_load(&stage), record->ExceptionCode, address, record->ExceptionAddress,
            (unsigned long long)last_function, (unsigned long long)last_lr);
    atomic_store(&stop_reason, "host_fault");
    report(); fflush(stdout); fflush(stderr);
    _Exit(78);
}
#else
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
#endif

static void guest_trace(Cpu *c) {
    uint64_t frame = c->x[29];
    for (unsigned i = 0; i < 32 && frame && !(frame & 7) && kernel_access(frame, 16, 1); i++) {
        uint64_t next = LD64(frame), lr = LD64(frame + 8);
        fprintf(stderr, "STARTUP caller lr=%llx\n", (unsigned long long)lr);
        if (next <= frame || next - frame > 0x100000) break;
        frame = next;
    }
}

static _Noreturn void stop_worker(void) {
#ifdef _WIN32
    // Other guest threads can still be in services. A fatal bounded worker
    // exits without running process-wide destructors concurrently with them.
    report(); fflush(stdout); fflush(stderr); _Exit(78);
#else
    exit(78);
#endif
}

void rt_report_missing(Cpu *c) {
    atomic_store(&stop_reason, "dispatch_miss");
    fprintf(stderr, "STARTUP dispatch miss stage=%s last_entered=%llx lr=%llx\n", stage,
            (unsigned long long)last_function, (unsigned long long)last_lr);
    guest_trace(c);
#ifdef _WIN32
    stop_worker();
#endif
}

void rt_report_missing_host_api(Cpu *c, const char *name) {
    atomic_store(&stop_reason, "unsupported_host_api");
    fprintf(stderr, "STARTUP missing host API: %s lr=%llx\n", name, (unsigned long long)c->x[30]);
    guest_trace(c);
#ifdef _WIN32
    stop_worker();
#endif
}

void rt_report_graphics_error(Cpu *c, const char *message) {
    atomic_store(&stop_reason, "graphics_backend_error");
    fprintf(stderr, "STARTUP graphics backend: %s\n", message);
    guest_trace(c);
#ifdef _WIN32
    stop_worker();
#endif
}

_Noreturn void rt_report_adapter_error(Cpu *c, const char *message) {
    atomic_store(&stop_reason, "adapter_guard_error");
    fprintf(stderr, "STARTUP adapter guard: %s\n", message);
    guest_trace(c);
    stop_worker();
    _Exit(78);
}

void rt_svc(Cpu *c, uint32_t number, uint64_t pc) {
    const char *reason = "unsupported_svc";
    threads_checkpoint();
    if (kernel_svc(number, c->x)) { threads_checkpoint(); return; }
#ifndef NSW_KERNEL_ONLY
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
        reason = "unsupported_service";
    }
    if (number == 0x21) {
        uint32_t result;
        if (service_request((uint32_t)c->x[0], (void *)(uintptr_t)c->tpidrro, 0x100, &result)) {
            c->x[0] = result;
            return;
        }
        fprintf(stderr, "STARTUP IPC stopped: %s\n", service_blocker());
        reason = "unsupported_ipc";
    }
#endif
    if (number == 0x26 && c->x[2] == 4 && kernel_access(c->x[1], 4, 1))
        fprintf(stderr, "STARTUP guest break result=%x\n", LD32(c->x[1]));
    guest_trace(c);
    fprintf(stderr, "STARTUP unsupported svc=%x stage=%s pc=%llx x0=%llx x1=%llx x2=%llx x3=%llx\n",
            number, stage, (unsigned long long)pc, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    atomic_store(&stop_reason, reason);
    stop_worker();
}

void rt_brk(Cpu *c, uint32_t immediate, uint64_t pc) {
    (void)c;
    fprintf(stderr, "STARTUP break stage=%s pc=%llx immediate=%x\n", stage, (unsigned long long)pc, immediate);
    atomic_store(&stop_reason, "guest_break");
    stop_worker();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    fprintf(stderr, "STARTUP unsupported instruction stage=%s pc=%llx\n", stage, (unsigned long long)pc);
    atomic_store(&stop_reason, "unsupported_instruction");
    stop_worker();
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
        atomic_store(&stop_reason, "unbalanced_stack");
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
#include "utf8_main.h"
int main(int argc, char **argv) {
#endif
    uint64_t through, entries[7];
    if (argc < 4 || !number(argv[2], &through) || !through || through > 7 || argc != (int)through + 3) return LOAD_INVALID;
    for (unsigned i = 0; i < through; i++)
        if (!number(argv[i + 3], &entries[i]) || !has_function(entries[i])) return LOAD_INVALID;
    if (rt_validate_tables()) return LOAD_INVALID;
#ifdef NSW_BASIC_SERVICES
    if (!service_set_access(getenv("NSW_SERVICE_ACCESS"))) return LOAD_INVALID;
    const char *program = getenv("NSW_PROGRAM_ID");
    if (program) {
        char *end; uint64_t value = strtoull(program, &end, 16);
        if (strlen(program) != 16 || strspn(program, "0123456789abcdefABCDEF") != 16 || *end || !service_set_program(value)) return LOAD_INVALID;
    }
#endif
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(fault);
    uint64_t timeout=30;
    if(getenv("TOTK_RUN_SECONDS") && (!number(getenv("TOTK_RUN_SECONDS"),&timeout) || timeout<1 || timeout>600)) return LOAD_INVALID;
    HANDLE timer=CreateThread(NULL,0,watchdog,(void *)(uintptr_t)timeout,0,NULL);
    if(!timer) return LOAD_INVALID;
    CloseHandle(timer);
#else
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core)) return LOAD_INVALID;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGALRM};
    for (unsigned i = 0; i < sizeof signals / sizeof *signals; i++)
        if (sigaction(signals[i], &action, NULL)) return LOAD_INVALID;
    unsigned timeout = 30;
    uint64_t requested;
    if (getenv("TOTK_RUN_SECONDS")) {
        if (!number(getenv("TOTK_RUN_SECONDS"), &requested) || requested < 1 || requested > 600) return LOAD_INVALID;
        timeout = (unsigned)requested;
    }
    alarm(timeout);
#endif
    LoadedImage image = {0};
    int result = load_plan(argv[1], &image, has_function);
    if (result) return result;
    size_t page = image.page_size, stack_size = 8 << 20, region_size = stack_size + 2 * page;
    unsigned char *stack = host_reserve_guest(region_size);
    if (!stack) { unload_image(&image); return LOAD_ADDRESS_UNAVAILABLE; }
    if (host_protect(stack + page, stack_size, 3)) {
        host_release(stack, region_size); unload_image(&image); return LOAD_INVALID;
    }
    cpu.sp = (uintptr_t)stack + page + stack_size;
    cpu.excl_addr = UINT64_MAX;
    uint32_t main_handle;
    result = kernel_init(&image, (uintptr_t)stack + page, stack_size, &main_handle);
    if (result) { host_release(stack, region_size); unload_image(&image); return result; }
    // Main-thread TLS must also belong to the guest map. A Windows host stack
    // can live above the 39-bit guest limit, and cannot serve as guest TLS/TLR.
    uint64_t tls = kernel_thread_local();
    if (!tls) {
        kernel_destroy(); host_release(stack, region_size); unload_image(&image); return LOAD_INVALID;
    }
    cpu.tpidrro = tls;
    cpu.tpidr = tls + 0x1000;
    ST64(tls + 0x1f8, tls + 0x200);
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
    if (!threads_stop()) {
        atomic_store(&stop_reason, "shutdown_timeout");
        return 78;
    }
#ifdef NSW_VULKAN
    vulkan_runtime_shutdown();
#endif
#ifndef NSW_KERNEL_ONLY
    service_shutdown();
#endif
    host_release(stack, region_size);
    kernel_destroy();
    unload_image(&image);
    atomic_store(&stop_reason, atomic_load(&window_closed) ? "window_closed" : "checkpoint");
    return 0;
}
