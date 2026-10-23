// Synthetic NVN command recording and Vulkan handoff. No guest program or GPU.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "vulkan_bridge.c"
#include "vulkan_draw_spirv.h"
typedef struct {
    NVNdeviceBuilder db; NVNdevice device; NVNqueueBuilder qb; NVNqueue queue;
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool; NVNtextureBuilder tb; NVNtexture texture,depth_texture;
    NVNcommandBuffer command,copy; NVNsync sync; const NVNtexture *colors[1];
    NVNtextureView view; int view_values[4];
    NVNprogram program; NvnShaderDataExt shader;
    NVNtexturePool texture_pool;
    NVNcopyRegion region;
    NVNcommandHandle handle; float rgba[4];
} Objects;
static Objects objects;
static unsigned char *storage;
static void *allocation;
static unsigned created, cleared, released, callbacks;
static unsigned depth_created,depth_cleared;
static VulkanColorFormat color_format;
static const char *mode;
const NvnBridgeEntry nvn_bridge_entries[]={{"nvnDeviceInitialize",NULL}};
const size_t nvn_bridge_count=1;
_Thread_local Cpu *nvn_bridge_cpu;
bool kernel_access(uint64_t address,size_t size,unsigned permissions) {
    (void)permissions;
    return (address>=(uintptr_t)&objects && address-(uintptr_t)&objects<=sizeof objects && size<=sizeof objects-(address-(uintptr_t)&objects)) ||
        (address>=(uintptr_t)storage && address-(uintptr_t)storage<=65536 && size<=65536-(address-(uintptr_t)storage));
}
uint64_t nvn_guest_address(const char *name) {(void)name; return 0;}
void rt_report_graphics_error(Cpu *c,const char *error) {(void)c; assert(error && error[0]);
    if(!strcmp(mode,"draw-boundary")) {assert(cleared==1); assert(strstr(error,"draw shader stage"));}
    fprintf(stderr,"expected boundary: %s\n",error);}
bool vulkan_runtime_ready(void) {return true;}
uint64_t vulkan_runtime_draw_count(void) {return 0;}
bool vulkan_runtime_draw(const VulkanDraw *d,void *p,size_t n,char e[256]) {(void)d;(void)p;(void)n;(void)e; abort();}
bool vulkan_runtime_pixels(VulkanTarget *t,bool u,void *p,size_t n,char e[256]) {(void)t;(void)u;(void)p;(void)n;(void)e; abort();}
bool vulkan_runtime_copy(VulkanTarget *s,VulkanTarget *d,bool l,void *p,size_t n,char e[256]) {(void)s;(void)d;(void)l;(void)p;(void)n;(void)e; abort();}
bool vulkan_runtime_compute(const uint32_t *s,size_t n,const uint32_t g[3],const VulkanComputeBuffer *b,unsigned c,char e[256]) {(void)s;(void)n;(void)g;(void)b;(void)c;(void)e; abort();}
bool vulkan_display_native(const void *token) {return token==(void *)1;}
VulkanTarget *vulkan_runtime_target(uint32_t width,uint32_t height,VulkanColorFormat format,char error[256]) {
    (void)error; assert(width==16 && height==8 && !created); color_format=format; created++; return (VulkanTarget *)1;
}
bool vulkan_runtime_clear(VulkanTarget *target,const float color[4],void *rgba,size_t capacity,char error[256]) {
    (void)error; assert(target==(void *)1 && created && capacity==16*8*4); cleared++;
    unsigned char *bytes=rgba;
    if(color_format==VULKAN_COLOR_RGB11F) {
        assert(color[0]==1 && color[1]==0.5f && color[2]==0.25f);
        for(unsigned i=0;i<capacity;i+=4) {uint32_t pixel=0x681c03c0; memcpy(bytes+i,&pixel,4);}
    } else for(unsigned i=0;i<capacity;i++) {float c=color[i%4]; if(color_format==VULKAN_COLOR_SRGBA8 && i%4<3) c=c<=0.0031308f?12.92f*c:1.055f*powf(c,1.f/2.4f)-0.055f; bytes[i]=(unsigned char)lroundf(c*255);}
    if(!strcmp(mode,"readback-mismatch")) bytes[31]^=255;
    return true;
}
VulkanTarget *vulkan_runtime_depth_target(uint32_t w,uint32_t h,char error[256]) {(void)error; assert(w==16 && h==8 && !depth_created); depth_created++; return (VulkanTarget *)2;}
bool vulkan_runtime_clear_depth(VulkanTarget *t,float d,bool cd,uint8_t s,uint8_t sm,void *p,size_t n,char e[256]) {
    (void)e; assert(t==(void *)2 && d==1 && cd && s==0 && sm==255 && p && n==16*8*5); depth_cleared++; return true;
}
bool vulkan_runtime_release(VulkanTarget *target) {assert((target==(void *)1 && created) || (target==(void *)2 && depth_created)); released++; return true;}
void nvn_guest_callback(uint64_t address,uint64_t a0,uint64_t event,uint64_t size,uint64_t data) {
    assert(address==1 && a0==(uintptr_t)&objects.command && event==NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY && size<8192 && data==2);
    callbacks++; assert(callbacks==1);
    assert(h_nvnCommandBufferGetCommandMemoryUsed(&objects.command)==24);
    h_nvnCommandBufferAddCommandMemory(&objects.command,&objects.pool,8192,8192);
}
static void cleanup(void) {free(allocation);}
static void check_window_origin(void) {
    CommandState state={0}; VulkanDraw draw={0};
    state.viewports[0][0]=7; state.viewports[0][1]=16; state.viewports[0][2]=64; state.viewports[0][3]=32;
    state.scissor=(typeof(state.scissor)){7,20,64,8};
    // NVN rectangle is 16..48 from the lower edge of a 128-row framebuffer.
    // Its NDC -1 and +1 endpoints must land on top-down rows 112 and 80.
    state.origin=NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT; draw_coordinates(&state,128,&draw);
    assert(draw.viewport.y==112 && draw.viewport.y+draw.viewport.height==80);
    assert(draw.scissor.offset.y==100 && draw.scissor.extent.height==8);
    // Changing only the origin places the same rectangle 16..48 from the top.
    state.origin=NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT; draw_coordinates(&state,128,&draw);
    assert(draw.viewport.y==16 && draw.viewport.y+draw.viewport.height==48);
    assert(draw.scissor.offset.y==20 && draw.scissor.extent.height==8);
}
int main(int argc,char **argv) {
    check_window_origin();
    uint32_t locations; char reflection_error[256];
    assert(vulkan_vertex_locations(synthetic_vertex_spirv,sizeof synthetic_vertex_spirv,&locations,reflection_error) && locations==1);
    assert(vulkan_vertex_locations(synthetic_fragment_spirv,sizeof synthetic_fragment_spirv,&locations,reflection_error) && locations==0);
    assert(!vulkan_vertex_locations(synthetic_vertex_spirv,24,&locations,reflection_error));
    assert(draw_blend_factor(NVN_BLEND_FUNC_SRC_ALPHA)==VK_BLEND_FACTOR_SRC_ALPHA);
    assert(draw_blend_factor(NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA)==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    assert(draw_blend_factor(NVN_BLEND_FUNC_DST_COLOR)==VK_BLEND_FACTOR_DST_COLOR);
    assert(draw_blend_factor(NVN_BLEND_FUNC_CONSTANT_ALPHA)==VK_BLEND_FACTOR_CONSTANT_ALPHA);
    assert(argc==2); mode=argv[1]; allocation=malloc(65536+4095); assert(allocation);
    storage=(void *)(((uintptr_t)allocation+4095)&~(uintptr_t)4095); assert(!atexit(cleanup));
    memset(storage,0,65536); Objects *o=&objects;
    builder_defaults(&o->db); assert(device_initialize(&o->device,&o->db));
    h_nvnQueueBuilderSetDevice(&o->qb,&o->device); h_nvnQueueBuilderSetDefaults(&o->qb);
    assert(h_nvnQueueBuilderGetQueueMemorySize(&o->qb)==344064); assert(h_nvnQueueInitialize(&o->queue,&o->qb));
    h_nvnMemoryPoolBuilderSetDevice(&o->pb,&o->device); h_nvnMemoryPoolBuilderSetDefaults(&o->pb);
    h_nvnMemoryPoolBuilderSetStorage(&o->pb,storage,65536); h_nvnMemoryPoolBuilderSetFlags(&o->pb,5);
    assert(h_nvnMemoryPoolInitialize(&o->pool,&o->pb));
    if(!strcmp(mode,"pool-overflow")) {h_nvnMemoryPoolFlushMappedRange(&o->pool,65535,2); assert(0);}
    h_nvnTextureBuilderSetDevice(&o->tb,&o->device); h_nvnTextureBuilderSetDefaults(&o->tb);
    h_nvnTextureBuilderSetSize2D(&o->tb,16,8); h_nvnTextureBuilderSetFormat(&o->tb,!strcmp(mode,"rgb11f")?NVN_FORMAT_R11G11B10F:!strcmp(mode,"srgb")?NVN_FORMAT_RGBA8_SRGB:NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&o->tb,&o->pool,32768); assert(h_nvnTextureInitialize(&o->texture,&o->tb));
    if(!strcmp(mode,"linear")) {
        uint8_t rgba[16*8*4]; for(unsigned i=0;i<sizeof rgba;i++) rgba[i]=(uint8_t)i;
        assert(!movie_write_rgba(&o->texture,rgba,15,8));
        assert(movie_write_rgba(&o->texture,rgba,16,8));
        assert(!memcmp(storage+32768+88,rgba+(3*16+2)*4,8));
        assert(!memcmp(storage+32768+160,rgba+(4*16+4)*4,8));
        memset(storage+32768,0,(size_t)texture_check(&o->texture)->L.size);
    }
    if(!strcmp(mode,"depth")) {h_nvnTextureBuilderSetFormat(&o->tb,NVN_FORMAT_DEPTH24_STENCIL8); h_nvnTextureBuilderSetStorage(&o->tb,&o->pool,36864); assert(h_nvnTextureInitialize(&o->depth_texture,&o->tb));}
    h_nvnTextureViewSetDefaults(&o->view); o->view_values[0]=123;
    assert(!h_nvnTextureViewGetLevels(&o->view,&o->view_values[0],&o->view_values[1]) && o->view_values[0]==123);
    h_nvnTextureViewSetLevels(&o->view,2,-1);
    assert(h_nvnTextureViewGetLevels(&o->view,&o->view_values[0],&o->view_values[1]) && o->view_values[0]==2 && o->view_values[1]==-1);
    h_nvnTextureViewSetLayers(&o->view,3,2);
    assert(h_nvnTextureViewGetLayers(&o->view,&o->view_values[0],&o->view_values[1]) && o->view_values[0]==3 && o->view_values[1]==2);
    assert(h_nvnTexturePoolInitialize(&o->texture_pool,&o->pool,40960,257));
    h_nvnTexturePoolRegisterTexture(&o->texture_pool,256,&o->texture,NULL);
    assert(descriptors[0].texture.serial==texture_check(&o->texture)->serial);
    assert(h_nvnProgramInitialize(&o->program,&o->device));
    uint32_t *control=(uint32_t *)(storage+16384); control[0]=0x98761234; control[0x20/4]=0x748; control[0x704/4]=16;
    memset(storage+24576,0x5a,16); o->shader.shader=(NVNshaderData){.control=control,.data=(uintptr_t)(storage+24576)};
    if(!strcmp(mode,"shader-extension")) o->shader.metadata=(void *)1;
    assert(h_nvnProgramSetShadersExt(&o->program,1,&o->shader)); assert(program_bytes==0x748+16);
    memset(storage+24576,0,16); assert(((unsigned char *)programs[0].blocks[0])[0x748]==0x5a);
    h_nvnProgramFinalize(&o->program); assert(!program_bytes);
    o->region=(NVNcopyRegion){.xoffset=2,.yoffset=3,.width=4,.height=2,.depth=1};
    for(unsigned i=0;i<32;i++) storage[16384+i]=(unsigned char)(i+1);
    if(!strcmp(mode,"texture-bounds")) o->region.width=100;
    h_nvnTextureWriteTexels(&o->texture,NULL,&o->region,storage+16384);
    // Independent GOB offsets: row 3 at x-byte 8/16 maps to 88/112,
    // row 4 maps to 136/160. Untouched storage remains zero.
    assert(!memcmp(storage+32768+88,storage+16384,8) && !memcmp(storage+32768+112,storage+16384+8,8));
    assert(!memcmp(storage+32768+136,storage+16384+16,8) && !memcmp(storage+32768+160,storage+16384+24,8));
    assert(!storage[32768+87] && !storage[32768+96]);
    assert(h_nvnCommandBufferInitialize(&o->command,&o->device));
    h_nvnCommandBufferAddCommandMemory(&o->command,&o->pool,0,!strcmp(mode,"callback")?32:8192);
    h_nvnCommandBufferSetMemoryCallback(&o->command,(PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC)1);
    h_nvnCommandBufferSetMemoryCallbackData(&o->command,(void *)2);
    h_nvnCommandBufferBeginRecording(&o->command);
    h_nvnCommandBufferSetTexturePool(&o->command,&o->texture_pool);
    h_nvnCommandBufferBindProgram(&o->command,NULL,63);
    h_nvnCommandBufferBindUniformBuffer(&o->command,NVN_SHADER_STAGE_FRAGMENT,2,(uintptr_t)(storage+16384),256);
    h_nvnCommandBufferBindTexture(&o->command,NVN_SHADER_STAGE_FRAGMENT,1,0x12345678);
    h_nvnCommandBufferSetDepthBounds(&o->command,1,0.25f,0.75f);
    h_nvnCommandBufferSetStencilRef(&o->command,NVN_FACE_FRONT_AND_BACK,17);
    h_nvnCommandBufferReportCounter(&o->command,NVN_COUNTER_TYPE_TIMESTAMP,(uintptr_t)(storage+16384));
    o->colors[0]=&o->texture; h_nvnCommandBufferSetRenderTargets(&o->command,1,o->colors,NULL,!strcmp(mode,"depth")?&o->depth_texture:NULL,NULL);
    o->rgba[0]=0.18f; o->rgba[1]=0.5f; o->rgba[2]=0.003f; o->rgba[3]=1;
    if(!strcmp(mode,"rgb11f")) {o->rgba[0]=1; o->rgba[2]=0.25f;}
    if(!strcmp(mode,"partial")) h_nvnCommandBufferSetScissor(&o->command,1,0,15,8);
    if(!strcmp(mode,"texture-clear")) {
        o->region=(NVNcopyRegion){.width=16,.height=8,.depth=1};
        h_nvnCommandBufferClearTexture(&o->command,&o->texture,NULL,&o->region,o->rgba,15);
    } else h_nvnCommandBufferClearColor(&o->command,0,o->rgba,15);
    if(!strcmp(mode,"draw-boundary")) h_nvnCommandBufferDrawArrays(&o->command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    if(!strcmp(mode,"depth")) h_nvnCommandBufferClearDepthStencil(&o->command,1,1,0,255);
    o->handle=h_nvnCommandBufferEndRecording(&o->command);
    if(!strcmp(mode,"copy")) {
        assert(h_nvnCommandBufferInitialize(&o->copy,&o->device));
        h_nvnCommandBufferAddCommandMemory(&o->copy,&o->pool,8192,8192);
        h_nvnCommandBufferBeginRecording(&o->copy); h_nvnCommandBufferCopyCommands(&o->copy,1,&o->handle);
        o->handle=h_nvnCommandBufferEndRecording(&o->copy);
        memset(storage,0,8192); // CopyCommands must survive source arena reuse.
    }
    assert(!created && !cleared); // Recording must never execute GPU work.
    memset(storage+16384,0xcc,16);
    if(!strcmp(mode,"truncated")) ((op_hdr *)(uintptr_t)o->handle)->size=0;
    o->rgba[0]=0; // Recorded values and texture metadata must be independent of later guest edits.
    ((tex_t *)&o->texture)->d.width=32;
    h_nvnQueueSubmitCommands(&o->queue,1,&o->handle); assert(created==1 && cleared==1);
    assert(queue_states[0].state.texture_pool.n==257);
    assert(queue_states[0].state.uniform[NVN_SHADER_STAGE_FRAGMENT][2].size==256);
    assert(queue_states[0].state.textures[NVN_SHADER_STAGE_FRAGMENT][1]==0x12345678);
    assert(queue_states[0].state.stencil[0][2]==17 && queue_states[0].state.stencil[1][2]==17);
    float bounds[2]; memcpy(bounds,queue_states[0].state.draw_states[6]+4,8); assert(bounds[0]==0.25f && bounds[1]==0.75f);
    uint64_t counter[2]; memcpy(counter,storage+16384,16); assert(!counter[0] && counter[1] && counter[1]!=0xccccccccccccccccull);
    assert(h_nvnSyncInitialize(&o->sync,&o->device)); assert(h_nvnSyncWait(&o->sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    h_nvnQueueFenceSync(&o->queue,&o->sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    assert(h_nvnSyncWait(&o->sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    h_nvnQueueSubmitCommands(&o->queue,1,&o->handle); assert(created==1 && cleared==2);
    op_hdr first_header=*(const op_hdr *)(uintptr_t)o->handle;
    h_nvnDeviceFinalizeCommandHandle(&o->device,o->handle);
    assert(!memcmp(&first_header,(void *)(uintptr_t)o->handle,sizeof first_header));
    if(!strcmp(mode,"depth")) assert(depth_created==1 && depth_cleared==2);
    h_nvnTextureFinalize(&o->texture); assert(released==1); h_nvnCommandBufferFinalize(&o->command);
    if(!strcmp(mode,"depth")) {h_nvnTextureFinalize(&o->depth_texture); assert(released==2);}
    if(!strcmp(mode,"copy")) h_nvnCommandBufferFinalize(&o->copy);
    h_nvnTexturePoolFinalize(&o->texture_pool); assert(!descriptors[0].address);
    h_nvnMemoryPoolFinalize(&o->pool); h_nvnQueueFinalize(&o->queue); h_nvnSyncFinalize(&o->sync);
    vulkan_nvn_shutdown(); assert(!program_bytes && !native_textures[0].serial);
    return 0;
}
