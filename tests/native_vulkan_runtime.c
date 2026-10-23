#include "vulkan_runtime.h"
#include "vulkan_windows.h"
#include "vulkan_render.h"
#include "cpu.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void hle__ZN2nv18InitializeGraphicsEPvm(Cpu *);
void hle__ZN2nv16FinalizeGraphicsEv(Cpu *);
static unsigned modules, contexts, reported;
static const char *mode;
static VulkanHostInfo info;
static unsigned renders, targets, pending;
static bool is(const char *text) { return !strcmp(mode, text); }
bool vulkan_render_draw(VulkanRender *r,const VulkanDraw *d,void *p,size_t n,char e[256]) {(void)r;(void)d;(void)p;(void)n;(void)e; abort();}
bool vulkan_render_pixels(VulkanRender *r,VulkanTarget *t,bool u,void *p,size_t n,char e[256]) {(void)r;(void)t;(void)u;(void)p;(void)n;(void)e; abort();}
bool vulkan_render_copy(VulkanRender *r,VulkanTarget *s,VulkanTarget *d,bool l,void *p,size_t n,char e[256]) {(void)r;(void)s;(void)d;(void)l;(void)p;(void)n;(void)e; abort();}
VulkanCompute *vulkan_compute_create(const VulkanDevice *d,char e[256]) {(void)d;(void)e; abort();}
bool vulkan_compute_dispatch(VulkanCompute *c,const uint32_t *s,size_t n,const uint32_t g[3],const VulkanComputeBuffer *b,unsigned count,char e[256]) {(void)c;(void)s;(void)n;(void)g;(void)b;(void)count;(void)e; abort();}
bool vulkan_compute_close(VulkanCompute *c) {assert(!c); return true;}
bool kernel_access(uint64_t address, size_t size, unsigned permissions) {
    return address == 0x70000000 && size == 4096 && permissions == 3 && !is("invalid-memory");
}
bool vulkan_windows_load(VulkanLoader *loader, char error[256]) {
    if (is("loader-failure")) { strcpy(error, "synthetic missing loader"); return false; }
    *loader = (VulkanLoader){.module = (void *)1}; modules++; return true;
}
void vulkan_windows_unload(VulkanLoader *loader) {
    assert(!contexts);
    if (loader->module) modules--;
    *loader = (VulkanLoader){0};
}
VulkanHost *vulkan_host_create(PFN_vkGetInstanceProcAddr loader, int adapter, char error[256]) {
    (void)loader; assert(modules && adapter == -1);
    if (is("device-failure")) { strcpy(error, "synthetic device failure"); return NULL; }
    contexts++; return (VulkanHost *)1;
}
bool vulkan_host_close(VulkanHost *host) { assert(!renders && !targets); if (host) { assert(contexts == 1); contexts--; } return true; }
bool vulkan_host_device(const VulkanHost *host, VulkanDevice *out) { assert(host && contexts); *out=(VulkanDevice){0}; return true; }
void vulkan_display_shutdown(void) {}
VulkanRender *vulkan_render_create(const VulkanDevice *device, char error[256]) {
    (void)device; (void)error; assert(contexts && !renders); renders++; return (VulkanRender *)2;
}
VulkanTarget *vulkan_render_target_format(VulkanRender *render, uint32_t width, uint32_t height, VulkanColorFormat format, char error[256]) {
    (void)error; assert(render && renders && width==16 && height==8 && format==VULKAN_COLOR_RGBA8); targets++; return (VulkanTarget *)3;
}
VulkanTarget *vulkan_render_depth_target(VulkanRender *render,uint32_t w,uint32_t h,char error[256]) {return vulkan_render_target_format(render,w,h,VULKAN_COLOR_RGBA8,error);}
size_t vulkan_render_target_pixels(VulkanRender *render,VulkanTarget *target) {assert(render && target); return 128;}
bool vulkan_render_clear_depth(VulkanRender *r,VulkanTarget *t,float d,bool cd,uint8_t s,uint8_t sm,void *p,size_t n,char e[256]) {(void)r;(void)t;(void)d;(void)cd;(void)s;(void)sm;(void)p;(void)n;(void)e; abort();}
bool vulkan_render_clear(VulkanRender *render, VulkanTarget *target, const float color[4], void *rgba, size_t size, char error[256]) {
    (void)error; assert(render && renders && target && targets && color[0]==1 && !rgba && !size); return true;
}
bool vulkan_render_release(VulkanRender *render, VulkanTarget *target) { assert(render && target && targets); targets--; return true; }
bool vulkan_render_close(VulkanRender *render) {
    if (!render) return true;
    assert(contexts && renders); if (pending) {pending--; return false;} renders=targets=0; return true;
}
const VulkanHostInfo *vulkan_host_info(const VulkanHost *host) { assert(host); return &info; }
void rt_report_graphics_error(Cpu *c, const char *error) { assert(c && error[0]); reported++; }
static void clean(void) {
    assert(!modules && !contexts);
    assert(reported == (unsigned)(is("invalid-memory") || is("loader-failure") || is("device-failure") || is("double-init")));
}

int main(int argc, char **argv) {
    assert(argc == 2); mode = argv[1]; assert(!atexit(clean));
    assert(!vulkan_runtime_device_created());
    assert(!vulkan_runtime_ready());
    Cpu c = {0}; c.x[0] = 0x70000000; c.x[1] = 4096; Cpu original = c;
    hle__ZN2nv18InitializeGraphicsEPvm(&c);
    assert(!memcmp(&c, &original, sizeof c) && vulkan_runtime_device_created() && vulkan_runtime_ready());
    if (is("double-init")) hle__ZN2nv18InitializeGraphicsEPvm(&c);
    if (is("finalize-reinitialize")) {
        hle__ZN2nv16FinalizeGraphicsEv(&c); hle__ZN2nv16FinalizeGraphicsEv(&c);
        assert(!contexts && !modules && !vulkan_runtime_ready() && vulkan_runtime_device_created());
        hle__ZN2nv18InitializeGraphicsEPvm(&c);
        assert(contexts == 1 && modules == 1);
    }
    if (is("render-lifetime")) {
        char error[256]; const float color[4]={1,0,0,1};
        VulkanTarget *target=vulkan_runtime_target(16,8,false,error); assert(target && renders && targets);
        assert(vulkan_runtime_clear(target,color,NULL,0,error)); assert(vulkan_runtime_release(target));
        target=vulkan_runtime_target(16,8,false,error); assert(target); pending=1;
        vulkan_runtime_shutdown(); assert(renders && targets && contexts && modules && vulkan_runtime_ready());
        vulkan_runtime_shutdown(); assert(!renders && !targets && !contexts && !modules && !vulkan_runtime_ready());
    }
    // atexit must close the device before unloading its function pointers.
    return 0;
}
