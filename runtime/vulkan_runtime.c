// Opt-in offscreen Vulkan runtime. All access to the host queue is serialized.
#include "vulkan_runtime.h"
#include "vulkan_windows.h"
#include "vulkan_render.h"
#include "vulkan_display.h"
#include "cpu.h"
#include "kernel.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static VulkanHost *host;
static VulkanRender *renderer;
static VulkanCompute *computer;
static VulkanLoader loader;
static atomic_bool created;
static atomic_uint_fast64_t clears, depth_clears, readback_pixels, dispatches,draws;
static bool cleanup_registered;
__attribute__((weak)) void vulkan_nvn_shutdown(void) {}
extern void rt_report_graphics_error(Cpu *, const char *);

bool vulkan_runtime_device_created(void) { return atomic_load(&created); }
uint64_t vulkan_runtime_clear_count(void) { return atomic_load(&clears); }
uint64_t vulkan_runtime_depth_clear_count(void) { return atomic_load(&depth_clears); }
uint64_t vulkan_runtime_dispatch_count(void) { return atomic_load(&dispatches); }
uint64_t vulkan_runtime_draw_count(void) {return atomic_load(&draws);}
bool vulkan_runtime_draw(const VulkanDraw *draw,void *pixels,size_t capacity,char error[256]) {
    pthread_mutex_lock(&lock); bool ok=vulkan_render_draw(renderer,draw,pixels,capacity,error); if(ok) atomic_fetch_add(&draws,1); pthread_mutex_unlock(&lock); return ok;
}
bool vulkan_runtime_pixels(VulkanTarget *target,bool upload,void *pixels,size_t capacity,char error[256]) {
    pthread_mutex_lock(&lock); bool ok=vulkan_render_pixels(renderer,target,upload,pixels,capacity,error); pthread_mutex_unlock(&lock); return ok;
}
bool vulkan_runtime_compute(const uint32_t *spirv,size_t bytes,const uint32_t groups[3],const VulkanComputeBuffer *buffers,unsigned count,char error[256]) {
    pthread_mutex_lock(&lock);
    if(!computer && host) {VulkanDevice device; if(vulkan_host_device(host,&device)) computer=vulkan_compute_create(&device,error); else snprintf(error,256,"Vulkan device is busy");}
    if(!host) snprintf(error,256,"Vulkan device is not initialized");
    bool ok=computer && vulkan_compute_dispatch(computer,spirv,bytes,groups,buffers,count,error);
    if(ok) atomic_fetch_add(&dispatches,1);
    pthread_mutex_unlock(&lock); return ok;
}
uint64_t vulkan_runtime_readback_pixels(void) { return atomic_load(&readback_pixels); }
bool vulkan_runtime_ready(void) {
    pthread_mutex_lock(&lock); bool ready = host != NULL; pthread_mutex_unlock(&lock); return ready;
}
static bool ensure_renderer(char error[256]) {
    if (!renderer && host) {
        VulkanDevice device;
        if (vulkan_host_device(host, &device)) renderer = vulkan_render_create(&device, error);
        else snprintf(error, 256, "Vulkan device is busy");
    } else if (!host) snprintf(error, 256, "Vulkan device is not initialized");
    return renderer!=NULL;
}
VulkanTarget *vulkan_runtime_target(uint32_t width, uint32_t height, VulkanColorFormat format, char error[256]) {
    pthread_mutex_lock(&lock);
    VulkanTarget *target = ensure_renderer(error) ? vulkan_render_target_format(renderer, width, height, format, error) : NULL;
    pthread_mutex_unlock(&lock); return target;
}
VulkanTarget *vulkan_runtime_depth_target(uint32_t width,uint32_t height,char error[256]) {
    pthread_mutex_lock(&lock);
    VulkanTarget *target=ensure_renderer(error)?vulkan_render_depth_target(renderer,width,height,error):NULL;
    pthread_mutex_unlock(&lock); return target;
}
bool vulkan_runtime_clear(VulkanTarget *target, const float color[4], void *rgba, size_t capacity, char error[256]) {
    pthread_mutex_lock(&lock);
    bool ok = vulkan_render_clear(renderer, target, color, rgba, capacity, error);
    if(ok) {atomic_fetch_add(&clears,1); if(rgba) atomic_fetch_add(&readback_pixels,vulkan_render_target_pixels(renderer,target));}
    pthread_mutex_unlock(&lock); return ok;
}
bool vulkan_runtime_clear_depth(VulkanTarget *target,float depth,bool clear_depth,uint8_t stencil,uint8_t stencil_mask,void *pixels,size_t capacity,char error[256]) {
    pthread_mutex_lock(&lock);
    bool ok=vulkan_render_clear_depth(renderer,target,depth,clear_depth,stencil,stencil_mask,pixels,capacity,error);
    if(ok) {atomic_fetch_add(&clears,1); atomic_fetch_add(&depth_clears,1); if(pixels) atomic_fetch_add(&readback_pixels,vulkan_render_target_pixels(renderer,target));}
    pthread_mutex_unlock(&lock); return ok;
}
bool vulkan_runtime_release(VulkanTarget *target) {
    pthread_mutex_lock(&lock); bool ok = vulkan_render_release(renderer, target); pthread_mutex_unlock(&lock); return ok;
}
bool vulkan_runtime_copy(VulkanTarget *source,VulkanTarget *destination,bool linear,void *pixels,size_t capacity,char error[256]) {
    pthread_mutex_lock(&lock); bool ok=vulkan_render_copy(renderer,source,destination,linear,pixels,capacity,error); pthread_mutex_unlock(&lock); return ok;
}

void vulkan_runtime_shutdown(void) {
    vulkan_display_shutdown();
    bool closed=false;
    pthread_mutex_lock(&lock);
    if (vulkan_compute_close(computer)) {
    computer=NULL;
    if (vulkan_render_close(renderer)) {
        renderer = NULL;
        if (vulkan_host_close(host)) {
        host = NULL;
        vulkan_windows_unload(&loader);
        closed=true;
        }
    }
    }
    pthread_mutex_unlock(&lock);
    if(closed) vulkan_nvn_shutdown();
}

static void stop(Cpu *c, const char *error) __attribute__((noreturn));
static void stop(Cpu *c, const char *error) {
    rt_report_graphics_error(c, error);
    exit(78);
}

void hle__ZN2nv18InitializeGraphicsEPvm(Cpu *c) {
    if (!c->x[0] || !c->x[1] || !kernel_access(c->x[0], c->x[1], 3))
        stop(c, "invalid NV graphics transfer memory");
    int adapter = -1;
    const char *selection = getenv("NSW_VULKAN_ADAPTER");
    if (selection) {
        char *end; long value = strtol(selection, &end, 10);
        if (!*selection || *end || value < -1 || value >= 32) stop(c, "invalid Vulkan adapter selection");
        adapter = (int)value;
    }
    char error[256];
    pthread_mutex_lock(&lock);
    if (host) { pthread_mutex_unlock(&lock); stop(c, "Vulkan graphics already initialized"); }
    if (!vulkan_windows_load(&loader, error)) { pthread_mutex_unlock(&lock); stop(c, error); }
    host = vulkan_host_create(loader.get_instance_proc, adapter, error);
    if (!host) {
        vulkan_windows_unload(&loader); pthread_mutex_unlock(&lock); stop(c, error);
    }
    if (!cleanup_registered) {
        if (atexit(vulkan_runtime_shutdown)) {
            pthread_mutex_unlock(&lock); vulkan_runtime_shutdown(); stop(c, "cannot register Vulkan cleanup");
        }
        cleanup_registered = true;
    }
    atomic_store(&created, true);
    const VulkanHostInfo *info = vulkan_host_info(host);
    fprintf(stderr, "GRAPHICS Vulkan headless device created: %s adapter=%u queue=%u\n",
        info->device.deviceName, info->adapter_index, info->queue_family);
    pthread_mutex_unlock(&lock);
}

void hle__ZN2nv16FinalizeGraphicsEv(Cpu *c) { (void)c; vulkan_runtime_shutdown(); }
