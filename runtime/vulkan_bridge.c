// Typed NVN entry points for the bounded Vulkan backend. Unknown calls stop.
#include "nvn_bridge.h"
#include "nvn_protos.h"
#include "nvn_extra_protos.h"
#include "host_compat.h"
#include "vulkan_runtime.h"
#include "nvn_obj.h"
#include <stdarg.h>

extern uint64_t nvn_guest_address(const char *name);
extern void rt_report_graphics_error(Cpu *, const char *);

void die(const char *format, ...) {
    char text[256]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof text, format, args); va_end(args);
    Cpu empty = {0};
    rt_report_graphics_error(nvn_bridge_cpu ? nvn_bridge_cpu : &empty, text);
    exit(78);
}
static void check(const void *address, size_t size, unsigned permission) {
    if (!kernel_access((uintptr_t)address, size, permission))
        die("NVN guest pointer outside accessible memory: address=%llx size=%zu permissions=%u",(unsigned long long)(uintptr_t)address,size,permission);
}
static void device_check(const NVNdevice *device) {
    check(device, sizeof(dev_t_), 1);
    if (((const dev_t_ *)device)->magic != MAGIC('V','K','D','V')) die("invalid NVN device");
}
static PFNNVNGENERICFUNCPTRPROC device_proc(const NVNdevice *device, const char *name) {
    (void)device; char copy[129];
    for (unsigned i = 0; i < sizeof copy; i++) {
        check(name + i, 1, 1); copy[i] = name[i];
        if (!copy[i]) return (PFNNVNGENERICFUNCPTRPROC)(uintptr_t)nvn_guest_address(copy);
    }
    die("NVN API name is too long");
}
static void device_integer(const NVNdevice *device, NVNdeviceInfo what, int *out) {
    (void)device; check(out, sizeof *out, 3);
    // Guest ABI allocation/alignment contracts. Optional rendering features are
    // disabled until the Vulkan backend implements them.
    switch (what) {
    case NVN_DEVICE_INFO_API_MAJOR_VERSION: *out = 55; break;
    case NVN_DEVICE_INFO_API_MINOR_VERSION: *out = 15; break;
    case NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS: case NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS: *out = 256; break;
    case NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE: case NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE: *out = 32; break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_POOL_SIZE: *out = 1048576; break;
    case NVN_DEVICE_INFO_MAX_SAMPLER_POOL_SIZE: *out = 4096; break;
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT: *out = 256; break;
    case NVN_DEVICE_INFO_MAX_UNIFORM_BUFFER_SIZE: *out = 65536; break;
    case NVN_DEVICE_INFO_COUNTER_ALIGNMENT: *out = 16; break;
    case NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS: *out = 8; break;
    case NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS: case NVN_DEVICE_INFO_VERTEX_ATTRIBUTES: *out = 16; break;
    case NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE: *out = 32; break;
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE: *out = 14; break;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM: *out = 2; break;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED: *out = 256; break;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_ALIGNMENT: *out = 4096; break;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_GRANULARITY: *out = 131072; break;
    case NVN_DEVICE_INFO_SUPPORTS_MIN_MAX_FILTERING: case NVN_DEVICE_INFO_SUPPORTS_STENCIL8_FORMAT:
    case NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS: case NVN_DEVICE_INFO_SUPPORTS_CONSERVATIVE_RASTER:
    case NVN_DEVICE_INFO_SUPPORTS_PASSTHROUGH_GEOMETRY_SHADERS: case NVN_DEVICE_INFO_SUPPORTS_VIEWPORT_SWIZZLE:
    case NVN_DEVICE_INFO_SUPPORTS_SPARSE_TILED_PACKAGED_TEXTURES: case NVN_DEVICE_INFO_SUPPORTS_ADVANCED_BLEND_MODES:
    case NVN_DEVICE_INFO_SUPPORTS_DRAW_TEXTURE: case NVN_DEVICE_INFO_SUPPORTS_TARGET_INDEPENDENT_RASTERIZATION:
    case NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_COVERAGE_TO_COLOR: case NVN_DEVICE_INFO_SUPPORTS_POST_DEPTH_COVERAGE:
    case NVN_DEVICE_INFO_SUPPORTS_IMAGES_USING_TEXTURE_HANDLES: case NVN_DEVICE_INFO_SUPPORTS_SAMPLE_LOCATIONS:
    case NVN_DEVICE_INFO_SUPPORTS_ZERO_FROM_UNMAPPED_VIRTUAL_POOL_PAGES:
    case NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_SHADER_INTERLOCK: case NVN_DEVICE_INFO_SUPPORTS_DEBUG_LAYER:
    case NVN_DEVICE_INFO_EVENTS_SUPPORT_REDUCTION_OPERATIONS: *out = 0; break;
    case NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE: *out = 65536; break;
    case NVN_DEVICE_INFO_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT: *out = 4096; break;
    case NVN_DEVICE_INFO_SHADER_CODE_MEMORY_POOL_PADDING_SIZE: *out = 1024; break;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_COMMAND_ALIGNMENT: *out = 4; break;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_CONTROL_ALIGNMENT: *out = 8; break;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE: *out = 512; break;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE: *out = 256; break;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_GRANULARITY: case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_GRANULARITY:
    case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_MIN_SIZE: case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD: *out = 4096; break;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE: case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_DEFAULT_SIZE: *out = 65536; break;
    case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_DEFAULT_SIZE: case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_MIN_SIZE: *out = 16384; break;
    case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_GRANULARITY: *out = 256; break;
    case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE: *out = 262144; break;
    case NVN_DEVICE_INFO_MAX_TEXTURES_PER_WINDOW: *out = 4; break;
    case NVN_DEVICE_INFO_MIN_TEXTURES_PER_WINDOW: *out = 2; break;
    default: die("Unimplemented NVN device property %u", (unsigned)what);
    }
    fprintf(stderr, "NVN property %u=%d\n", (unsigned)what, *out);
}
static void builder_defaults(NVNdeviceBuilder *builder) {
    check(builder, sizeof *builder, 3); memset(builder, 0, sizeof *builder);
}
static void builder_flags(NVNdeviceBuilder *builder, int flags) {
    check(builder, sizeof *builder, 3); memcpy(builder, &flags, sizeof flags);
}
static NVNboolean device_initialize(NVNdevice *device, const NVNdeviceBuilder *builder) {
    check(device, sizeof *device, 3); check(builder, sizeof *builder, 1);
    if (!vulkan_runtime_ready()) die("NVN device requires an initialized Vulkan host");
    memset(device, 0, sizeof *device);
    *(dev_t_ *)device = (dev_t_){.magic = MAGIC('V','K','D','V'), .flags = ((const dev_builder_t *)builder)->flags};
    fprintf(stderr, "NVN device initialized on Vulkan\n");
    return 1;
}

#include "vulkan_nvn.inc"

bool movie_write_rgba(void *texture,const uint8_t *rgba,unsigned width,unsigned height) {
    pthread_mutex_lock(&submit_lock);
    const tex_t *t=texture_check(texture);
    if(!rgba || !width || !height || width!=t->d.width || height!=t->d.height || t->d.depth!=1 || t->d.target!=NVN_TEXTURE_TARGET_2D ||
        t->d.levels!=1 || t->d.samples>1 || (t->d.format!=NVN_FORMAT_RGBA8 && t->d.format!=NVN_FORMAT_RGBA8_SRGB) || t->L.size>(64u<<20)) {
        pthread_mutex_unlock(&submit_lock); return false;
    }
    pool_range((const NVNmemoryPool *)t->pool,t->off,t->L.size,3);
    texture_invalidate_native(t->serial);
    if(t->d.flags&0x50) {
        size_t stride=t->L.size/height;
        if(stride<(size_t)width*4) {pthread_mutex_unlock(&submit_lock); return false;}
        for(unsigned y=0;y<height;y++) memcpy(t->mem+y*stride,rgba+(size_t)y*width*4,(size_t)width*4);
    } else if(nvn_swizzle(&t->d,0,0,rgba,(size_t)width*4,t->mem)) {pthread_mutex_unlock(&submit_lock); return false;}
    pthread_mutex_unlock(&submit_lock);
    fprintf(stderr,"MOVIE texture upload: %ux%u format=%u\n",width,height,t->d.format);
    return true;
}

// Called only after the renderer and device have released all GPU references.
void vulkan_nvn_shutdown(void) {
    pthread_mutex_lock(&program_lock);
    for(unsigned i=0;i<256;i++) program_drop(&programs[i]);
    pthread_mutex_unlock(&program_lock);
    pthread_mutex_lock(&texture_lock); memset(native_textures,0,sizeof native_textures); pthread_mutex_unlock(&texture_lock);
    pthread_mutex_lock(&descriptor_lock); memset(descriptors,0,sizeof descriptors); pthread_mutex_unlock(&descriptor_lock);
    pthread_mutex_lock(&submit_lock); memset(queue_states,0,sizeof queue_states); pthread_mutex_unlock(&submit_lock);
}

void *nvn_backend_native(size_t index) {
    if (index >= nvn_bridge_count) return NULL;
    const char *name = nvn_bridge_entries[index].name;
#define ENTRY(symbol, function) do { NVNT_##symbol typed = function; if (!strcmp(name, #symbol)) return (void *)typed; } while (0)
    ENTRY(nvnDeviceGetProcAddress, device_proc);
    ENTRY(nvnDeviceGetInteger, device_integer);
    ENTRY(nvnDeviceBuilderSetDefaults, builder_defaults);
    ENTRY(nvnDeviceBuilderSetFlags, builder_flags);
    ENTRY(nvnDeviceInitialize, device_initialize);
    // The same typed registration list is used by the bounded NVN implementation.
#define NVN_ENTRY(symbol) ENTRY(symbol, h_##symbol);
#include "vulkan_nvn_entries.inc"
#undef NVN_ENTRY
#undef ENTRY
    return NULL;
}
