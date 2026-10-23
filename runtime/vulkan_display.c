// VI objects and an optional Windows preview of completed Vulkan readbacks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "vulkan_display.h"
#include "vulkan_runtime.h"
#include "guest_callback.h"
#include "host_compat.h"
#include <stdatomic.h>

static uint32_t display, layer, native, vsync_write, vsync_read;
static pthread_t ticker;
static atomic_bool running;
static bool initialized, cleanup_registered;
static bool display_open, layer_open;
static int32_t crop[4];
static pthread_mutex_t preview_lock = PTHREAD_MUTEX_INITIALIZER;
static HostWindow *preview;
static atomic_bool preview_created, preview_closed;
static atomic_uint_fast64_t preview_submitted, preview_painted;

bool vulkan_display_window_requested(void) {
    const char *value = getenv("NSW_VULKAN_WINDOW");
    return value && !strcmp(value, "1");
}
static HostWindowStats preview_snapshot(void) {
    HostWindowStats stats = host_window_stats(preview);
    if (preview) {
        atomic_store(&preview_created, stats.created); atomic_store(&preview_closed, stats.closed);
        atomic_store(&preview_submitted, stats.submitted); atomic_store(&preview_painted, stats.painted);
    }
    return stats;
}
HostWindowStats vulkan_display_window_stats(void) {
    if (!pthread_mutex_trylock(&preview_lock)) {
        preview_snapshot(); pthread_mutex_unlock(&preview_lock);
    }
    return (HostWindowStats){.created = atomic_load(&preview_created), .closed = atomic_load(&preview_closed),
        .submitted = atomic_load(&preview_submitted), .painted = atomic_load(&preview_painted)};
}
bool vulkan_display_frame(const void *rgba, size_t bytes, uint32_t width, uint32_t height,
                          bool *queued, char error[256]) {
    pthread_mutex_lock(&preview_lock);
    uint64_t before = host_window_stats(preview).submitted;
    bool ok = !preview || host_window_frame(preview, rgba, bytes, width, height, error);
    *queued = preview_snapshot().submitted > before;
    pthread_mutex_unlock(&preview_lock);
    return ok;
}
extern void startup_window_closed(void);
static void preview_user_closed(void *unused) {
    (void)unused;
    startup_window_closed(); // Stops guest threads and emits the distinct close receipt.
    fflush(stdout); fflush(stderr);
    _Exit(0); // The guest main thread parks at its safe point until process exit.
}

static void *tick(void *unused) {
    (void)unused;
    for (unsigned count = 0; atomic_load(&running); count++) {
        Sleep(count % 3 ? 17 : 16);
        if (!atomic_load(&running)) break;
        uint64_t regs[31] = {vsync_write}; kernel_svc(0x11, regs);
    }
    return NULL;
}
void vulkan_display_shutdown(void) {
    pthread_mutex_lock(&preview_lock);
    preview_snapshot();
    if (!host_window_close(preview)) die("Windows preview did not stop within its shutdown bound");
    preview = NULL;
    pthread_mutex_unlock(&preview_lock);
    if (atomic_exchange(&running, false)) pthread_join(ticker, NULL);
    uint64_t regs[31] = {vsync_read}; if (vsync_read) kernel_svc(0x16, regs);
    regs[0] = vsync_write; if (vsync_write) kernel_svc(0x16, regs);
    vsync_write = vsync_read = 0; initialized = display_open = layer_open = false;
}
bool vulkan_display_native(const void *token) { return initialized && layer_open && token == &native; }
static void output(Cpu *c, void *value) {
    if (!initialized || !kernel_access(c->x[0], 8, 3)) { c->x[0] = 0x202; return; }
    ST64(c->x[0], (uintptr_t)value); c->x[0] = 0;
}
void hle__ZN2nn2vi10InitializeEv(Cpu *c) {
    (void)c;
    if (initialized) return;
    if (!vulkan_runtime_ready()) die("Display requires Vulkan graphics initialization");
    if (vulkan_display_window_requested()) {
        char error[256];
        pthread_mutex_lock(&preview_lock);
        bool opened = host_window_open(&preview, 960, 540, preview_user_closed, NULL, error);
        preview_snapshot(); pthread_mutex_unlock(&preview_lock);
        if (!opened) { vulkan_display_shutdown(); die("Windows preview: %s", error); }
    }
    if (!kernel_event_create(&vsync_write, &vsync_read)) die("Cannot allocate the offscreen display event");
    atomic_store(&running, true);
    if (pthread_create(&ticker, NULL, tick, NULL)) {
        atomic_store(&running, false); vulkan_display_shutdown(); die("Cannot start the offscreen display clock");
    }
    if (!cleanup_registered) {
        if (atexit(vulkan_display_shutdown)) { vulkan_display_shutdown(); die("Cannot register display cleanup"); }
        cleanup_registered = true;
    }
    initialized = true;
    fprintf(stderr, "DISPLAY Vulkan offscreen display initialized\n");
}
void hle__ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE(Cpu *c) { output(c, &display); if(!c->x[0]) display_open=true; }
void hle__ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE(Cpu *c) {
    if (!display_open || c->x[1] != (uintptr_t)&display) { c->x[0] = 0x202; return; }
    output(c, &layer);
    if(!c->x[0]) {layer_open=true; memset(crop,0,sizeof crop);}
}
void hle__ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii(Cpu *c) {
    if(!layer_open || c->x[0]!=(uintptr_t)&layer) {c->x[0]=0x202; return;}
    for(unsigned i=0;i<4;i++) if((int32_t)c->x[i+1]<0) {c->x[0]=0x202; return;}
    for(unsigned i=0;i<4;i++) crop[i]=(int32_t)c->x[i+1]; c->x[0]=0;
}
void hle__ZN2nn2vi12DestroyLayerEPNS0_5LayerE(Cpu *c) {
    if(!layer_open || c->x[0]!=(uintptr_t)&layer) {c->x[0]=0x202; return;}
    layer_open=false; c->x[0]=0;
}
void hle__ZN2nn2vi12CloseDisplayEPNS0_7DisplayE(Cpu *c) {
    if(!display_open || layer_open || c->x[0]!=(uintptr_t)&display) {c->x[0]=0x202; return;}
    display_open=false; c->x[0]=0;
}
void hle__ZN2nn2vi8FinalizeEv(Cpu *c) { (void)c; vulkan_display_shutdown(); }
void hle__ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(Cpu *c) {
    if (!layer_open || c->x[1] != (uintptr_t)&layer) { c->x[0] = 0x202; return; }
    output(c, &native);
}
void hle__ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE(Cpu *c) {
    if (!display_open || c->x[1] != (uintptr_t)&display || !kernel_access(c->x[0], 0x40, 3)) { c->x[0] = 0x202; return; }
    const char *text = getenv("NSW_VSYNC_ATTACH"); char *end;
    uint64_t address = text ? strtoull(text, &end, 16) : 0;
    if (!address || *end || !guest_callback_address(address)) die("Missing compiled SystemEvent attachment helper");
    uint32_t handle = kernel_event_copy(vsync_read);
    if (!handle) { c->x[0] = 0xce01; return; }
    uint64_t arguments[] = {c->x[0], handle, 1, 1};
    if (!guest_callback(c, address, arguments, 4, NULL)) die("Invalid SystemEvent attachment callback");
    c->x[0] = 0;
}
