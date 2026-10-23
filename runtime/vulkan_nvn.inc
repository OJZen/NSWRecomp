// Portable NVN resource contracts. Layouts and defaults follow the locally
// maintained Zelda-Mobile interface (see third_party/zelda-mobile/SOURCE.json).
// Queue submission is synchronous; unsupported GPU operations remain fatal.
#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)
#define WRITE(type, p) (check((p), sizeof(type), 3), (type *)(p))
#define READ(type, p) (check((p), sizeof(type), 1), (const type *)(p))
static void command_forget(NVNqueue *q);
static queue_t *queue_check(NVNqueue *q) {
    queue_t *o = WRITE(queue_t, q);
    if (o->magic != MAGIC('V','K','Q','U')) die("invalid NVN queue");
    device_check(o->dev); return o;
}
static const pool_t *pool_check(const NVNmemoryPool *p) {
    const pool_t *o = READ(pool_t, p);
    if (o->magic != MAGIC('V','K','P','L')) die("invalid NVN memory pool");
    return o;
}
typedef struct {NVNdevice *device; NVNmemoryPool *pool; ptrdiff_t offset; size_t size;} BufferBuilder;
typedef struct {uint32_t magic; NVNmemoryPool *pool; ptrdiff_t offset; size_t size;} Buffer;
FITS(BufferBuilder,NVNbufferBuilder); FITS(Buffer,NVNbuffer);
static const Buffer *buffer_check(const NVNbuffer *buffer) {
    const Buffer *o=READ(Buffer,buffer); if(o->magic!=MAGIC('V','K','B','F')) die("invalid NVN buffer"); pool_check(o->pool); return o;
}
static void *pool_range(const NVNmemoryPool *p, ptrdiff_t offset, size_t size, unsigned permissions) {
    const pool_t *o = pool_check(p);
    if (offset < 0 || (uint64_t)offset > o->size || size > o->size - offset) die("NVN memory pool range overflow");
    void *address = o->mem + offset; if (size) check(address, size, permissions); return address;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode mode) {
    device_check(d); if ((unsigned)mode > 1) die("invalid NVN origin"); WRITE(dev_t_, d)->origin = mode;
}
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode mode) {
    device_check(d); if ((unsigned)mode > 1) die("invalid NVN depth mode"); WRITE(dev_t_, d)->depth_mode = mode;
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { device_check(d); WRITE(queue_builder_t, b)->dev = d; }
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    queue_builder_t *o = WRITE(queue_builder_t, b); NVNdevice *d = o->dev;
    *o = (queue_builder_t){.dev=d, .cmd_sz=65536, .comp_sz=262144, .ctl_sz=16384, .flush=4096};
}
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { WRITE(queue_builder_t, b)->flags=f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { WRITE(queue_builder_t, b)->cmd_sz=n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { WRITE(queue_builder_t, b)->comp_sz=n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { WRITE(queue_builder_t, b)->ctl_sz=n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { WRITE(queue_builder_t, b)->flush=n; }
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = READ(queue_builder_t, b);
    if (o->cmd_sz > 128*1024*1024 || o->comp_sz > 128*1024*1024 || o->ctl_sz > 128*1024*1024)
        die("NVN queue memory exceeds bounded backend limit");
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *memory, size_t size) {
    if (size) check(memory, size, 3); queue_builder_t *o=WRITE(queue_builder_t, b); o->mem=memory; o->mem_sz=size;
}
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    const queue_builder_t *s=READ(queue_builder_t,b); device_check(s->dev);
    if (s->mem && s->mem_sz < h_nvnQueueBuilderGetQueueMemorySize(b)) return 0;
    command_forget(q); *WRITE(queue_t,q)=(queue_t){.magic=MAGIC('V','K','Q','U'), .dev=s->dev};
    fprintf(stderr,"NVN synchronous Vulkan queue initialized\n"); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { queue_check(q)->magic=0; command_forget(q); }
NVN(nvnQueueFlush, void, NVNqueue *q) { queue_check(q); }
NVN(nvnQueueFinish, void, NVNqueue *q) { queue_check(q); }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    queue_check(q); if (info) {check(info,sizeof *info,3); memset(info,0,sizeof *info);} return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { device_check(d); WRITE(pool_builder_t,b)->dev=d; }
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    pool_builder_t *o=WRITE(pool_builder_t,b); NVNdevice *d=o->dev; *o=(pool_builder_t){.dev=d};
}
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *memory, size_t size) {
    pool_builder_t *o=WRITE(pool_builder_t,b); o->mem=memory; o->size=size;
}
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int flags) { WRITE(pool_builder_t,b)->flags=flags; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s=READ(pool_builder_t,b); device_check(s->dev);
    if (s->flags & (0x100|0x200)) die("Vulkan virtual/physical NVN pools are unsupported");
    if (!s->size || (s->size & 4095) || ((uintptr_t)s->mem & 4095)) return 0;
    check(s->mem,s->size,3);
    *WRITE(pool_t,p)=(pool_t){.magic=MAGIC('V','K','P','L'),.flags=s->flags,.mem=s->mem,.size=s->size}; return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) { pool_check(p); WRITE(pool_t,p)->magic=0; }
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return pool_check(p)->mem; }
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (uintptr_t)pool_check(p)->mem; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return pool_check(p)->size; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return pool_check(p)->flags; }
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t offset, size_t size) { pool_range(p,offset,size,1); }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t offset, size_t size) { pool_range(p,offset,size,3); }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b,NVNdevice *d) {device_check(d); WRITE(BufferBuilder,b)->device=d;}
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) {BufferBuilder *o=WRITE(BufferBuilder,b); NVNdevice *d=o->device; *o=(BufferBuilder){.device=d};}
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b,NVNmemoryPool *p,ptrdiff_t offset,size_t size) {BufferBuilder *o=WRITE(BufferBuilder,b); o->pool=p; o->offset=offset; o->size=size;}
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buffer,const NVNbufferBuilder *b) {
    const BufferBuilder *s=READ(BufferBuilder,b); device_check(s->device); pool_range(s->pool,s->offset,s->size,3);
    *WRITE(Buffer,buffer)=(Buffer){.magic=MAGIC('V','K','B','F'),.pool=s->pool,.offset=s->offset,.size=s->size}; return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *b) {buffer_check(b); WRITE(Buffer,b)->magic=0;}
NVN(nvnBufferMap, void *, const NVNbuffer *b) {const Buffer *o=buffer_check(b); return pool_range(o->pool,o->offset,o->size,3);}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) {return (uintptr_t)h_nvnBufferMap(b);}
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b) {return buffer_check(b)->size;}
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b) {return buffer_check(b)->offset;}
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b) {return buffer_check(b)->pool;}
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b,ptrdiff_t offset,size_t size) {
    const Buffer *o=buffer_check(b); if(offset<0 || (uint64_t)offset>o->size || size>o->size-offset) die("NVN buffer range overflow");
    pool_range(o->pool,o->offset+offset,size,1);
}
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b,ptrdiff_t offset,size_t size) {h_nvnBufferFlushMappedRange(b,offset,size);}
static const sync_t *sync_check(const NVNsync *sync) {
    const sync_t *o=READ(sync_t,sync); if(o->magic!=MAGIC('V','K','S','Y')) die("invalid NVN sync"); return o;
}
NVN(nvnSyncInitialize, NVNboolean, NVNsync *sync, NVNdevice *device) {
    device_check(device); *WRITE(sync_t,sync)=(sync_t){.magic=MAGIC('V','K','S','Y')}; return 1;
}
NVN(nvnSyncFinalize, void, NVNsync *sync) { sync_check(sync); WRITE(sync_t,sync)->magic=0; }
NVN(nvnQueueFenceSync, void, NVNqueue *queue, NVNsync *sync, NVNsyncCondition condition, int flags) {
    queue_t *q=queue_check(queue); sync_check(sync);
    if ((unsigned)condition>1 || (flags & ~1)) die("unsupported NVN fence options");
    sync_t *o=WRITE(sync_t,sync); o->fence=q->submits; __atomic_store_n(&o->fenced,1,__ATOMIC_RELEASE);
}
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *sync, uint64_t timeout) {
    (void)timeout; return __atomic_load_n(&sync_check(sync)->fenced,__ATOMIC_ACQUIRE) ? NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *queue, const NVNsync *sync) {
    queue_check(queue); if (!__atomic_load_n(&sync_check(sync)->fenced,__ATOMIC_ACQUIRE)) die("NVN queue wait requires a submitted fence"); return 1;
}

static cmd_t *command_check(NVNcommandBuffer *command) {
    cmd_t *o=WRITE(cmd_t,command); if(o->magic!=MAGIC('V','K','C','B')) die("invalid NVN command buffer"); return o;
}
static const cmd_t *command_read(const NVNcommandBuffer *command) {
    const cmd_t *o=READ(cmd_t,command); if(o->magic!=MAGIC('V','K','C','B')) die("invalid NVN command buffer"); return o;
}
NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *command, NVNdevice *device) {
    device_check(device); *WRITE(cmd_t,command)=(cmd_t){.magic=MAGIC('V','K','C','B'),.dev=device}; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *command) {
    cmd_t *o=command_check(command); if(o->recording) die("NVN finalize during recording"); o->magic=0;
}
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *command, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { command_check(command)->cb=cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *command, void *data) { command_check(command)->cb_data=data; }
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *command, const NVNmemoryPool *pool, ptrdiff_t offset, size_t size) {
    cmd_t *o=command_check(command); uint8_t *mem=pool_range(pool,offset,size,3);
    if (((uintptr_t)mem & 7) || size < 32 || size > (128u<<20)) die("invalid NVN command memory");
    if (o->recording && o->begin && o->cur) {
        if ((size_t)(o->end-o->cur)<16) die("NVN command jump has no reserved tail");
        op_hdr jump={OP_JUMP,16}; memcpy(o->cur,&jump,8); memcpy(o->cur+8,&mem,8);
    }
    o->memory_start=o->cur=mem; o->end=mem+size; o->guest_size=size; o->guest_used=0;
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *command, void *memory, size_t size) {
    cmd_t *o=command_check(command); if (((uintptr_t)memory&7) || size<256) die("invalid NVN control memory");
    check(memory,size,3); o->control_size=size;
}
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return command_read(c)->guest_size; }
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *o=command_read(c); return o->guest_used+(o->command_callback_pending?16:0); }
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *o=command_read(c); return o->guest_size-h_nvnCommandBufferGetCommandMemoryUsed(c); }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return command_read(c)->control_size; }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { command_read(c); return 0; }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { return command_read(c)->control_size; }
extern void nvn_guest_callback(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static void *command_record(NVNcommandBuffer *c, unsigned op, size_t payload) {
    cmd_t *o=command_check(c); size_t size=(payload+8+7)&~(size_t)7;
    if (!o->recording || size > (1u<<20)) die("invalid NVN recording");
    for (unsigned tries=0; !o->cur || (size_t)(o->end-o->cur)<size+16; tries++) {
        if (tries==2 || !o->cb) die("NVN command memory exhausted");
        o->command_callback_pending=o->cur && o->begin;
        nvn_guest_callback((uintptr_t)o->cb,(uintptr_t)c,NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,size+16,(uintptr_t)o->cb_data);
        o->command_callback_pending=0;
    }
    check(o->cur,size,3); uint8_t *p=o->cur; o->cur+=size; o->guest_used+=size;
    op_hdr header={op,(uint32_t)size}; memcpy(p,&header,8); memset(p+8,0,size-8); return p+8;
}
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o=command_check(c); if(o->recording) die("nested NVN recording");
    o->recording=1; o->begin=NULL; o->begin=(uint8_t *)command_record(c,OP_BEGIN,0)-8;
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o=command_check(c); command_record(c,OP_END,0); o->recording=0; return (uintptr_t)o->begin;
}
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *device,NVNcommandHandle handle) {
    device_check(device); (void)handle;
    // Handles borrow guest command memory; they allocate no host resources.
    // Synchronous submission has already released its temporary GPU objects.
    // The guest owns arena reuse, so finalization must not overwrite the stream.
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return command_read(c)->recording!=0; }
static void debug_label(const char *label) {
    if (!label) return;
    for (unsigned i=0;i<1024;i++) {check(label+i,1,1); if(!label[i]) return;}
    die("NVN debug label exceeds bound");
}
NVN(nvnDeviceSetDebugLabel, void, NVNdevice *d, const char *label) { device_check(d); debug_label(label); }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *label) { queue_check(q); debug_label(label); }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *label) { pool_check(p); debug_label(label); }
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *label) { command_check(c); debug_label(label); }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *label) { sync_check(s); debug_label(label); }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *b,const char *label) {buffer_check(b); debug_label(label);}

static void descriptor_forget(const dpool_t *pool);
static NVNboolean descriptor_initialize(void *destination, const NVNmemoryPool *p, ptrdiff_t offset, int count, int limit) {
    if (count<256 || count>limit || (offset&31)) return 0;
    pool_range(p,offset,(size_t)count*32,3);
    *WRITE(dpool_t,destination)=(dpool_t){.pool=(const pool_t *)p,.off=offset,.n=count}; descriptor_forget(destination); return 1;
}
static const dpool_t *descriptor_check(const void *p) {
    const dpool_t *o=READ(dpool_t,p); if(o->n<256 || o->n>1048576) die("invalid NVN descriptor pool");
    pool_range((const NVNmemoryPool *)o->pool,o->off,(size_t)o->n*32,1); return o;
}
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *p, const NVNmemoryPool *memory, ptrdiff_t offset, int count) { return descriptor_initialize(p,memory,offset,count,1048576); }
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *p, const NVNmemoryPool *memory, ptrdiff_t offset, int count) { return descriptor_initialize(p,memory,offset,count,4096); }
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *p) { descriptor_forget(descriptor_check(p)); WRITE(dpool_t,p)->n=0; }
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *p) { descriptor_forget(descriptor_check(p)); WRITE(dpool_t,p)->n=0; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *label) { descriptor_check(p); debug_label(label); }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *label) { descriptor_check(p); debug_label(label); }
#include "vulkan_texture.inc"
#include "vulkan_texture_io.inc"
#include "vulkan_descriptor.inc"
#include "vulkan_program.inc"
#include "vulkan_display.h"
static win_t *window_check(NVNwindow *window) {
    win_t *o=WRITE(win_t,window); if(o->magic!=MAGIC('V','K','W','N') || !vulkan_display_native(o->native)) die("invalid NVN offscreen window"); return o;
}
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { device_check(d); WRITE(win_builder_t,b)->dev=d; }
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) {
    win_builder_t *o=WRITE(win_builder_t,b); NVNdevice *d=o->dev; *o=(win_builder_t){.dev=d,.interval=1};
}
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow native) {
    if(!vulkan_display_native((void *)native)) die("NVN window requires an offscreen VI layer"); WRITE(win_builder_t,b)->native=(void *)native;
}
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int count, NVNtexture *const *textures) {
    if(count<2 || count>4) die("invalid NVN window texture count"); check(textures,count*sizeof *textures,1);
    win_builder_t *o=WRITE(win_builder_t,b); o->ntex=count;
    for(int i=0;i<count;i++) {texture_check(textures[i]); o->tex[i]=textures[i];}
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int interval) {
    if(interval<0 || interval>5) die("invalid NVN present interval"); WRITE(win_builder_t,b)->interval=interval;
}
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *window, const NVNwindowBuilder *b) {
    const win_builder_t *s=READ(win_builder_t,b); device_check(s->dev);
    if(!vulkan_display_native(s->native) || s->ntex<2 || s->ntex>4) return 0;
    win_t *o=WRITE(win_t,window); *o=(win_t){.magic=MAGIC('V','K','W','N'),.native=s->native,.ntex=s->ntex,.interval=s->interval};
    for(int i=0;i<s->ntex;i++) {const tex_t *t=texture_check(s->tex[i]); if(!(t->d.flags&NVN_TEXTURE_FLAGS_DISPLAY)) die("NVN window texture lacks display flag"); o->tex[i]=s->tex[i];}
    fprintf(stderr,"NVN offscreen window initialized with %d textures\n",o->ntex); return 1;
}
NVN(nvnWindowFinalize, void, NVNwindow *window) { window_check(window)->magic=0; }
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *label) { window_check(w); debug_label(label); }
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) {return window_check((NVNwindow *)w)->interval;}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w,int interval) {
    if(interval<0 || interval>5) die("invalid NVN present interval"); window_check(w)->interval=interval;
}
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int width, int height) {
    win_t *o=window_check(w); if(x<0 || y<0 || width<0 || height<0) die("invalid NVN window crop");
    o->crop[0]=x; o->crop[1]=y; o->crop[2]=width; o->crop[3]=height;
}
static int window_acquire(NVNwindow *w) {
    win_t *o=window_check(w); int i=o->next++%o->ntex;
    if(o->ticket[i]) die("NVN offscreen image must be released by presentation before reuse");
    o->ticket[i]=1; return i;
}
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *sync, int *index) {
    sync_check(sync); check(index,sizeof *index,3); *index=window_acquire(w); __atomic_store_n(&WRITE(sync_t,sync)->fenced,1,__ATOMIC_RELEASE); return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *index) {
    queue_check(q); check(index,sizeof *index,3); *index=window_acquire(w); return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
#include "vulkan_state.inc"
#include "vulkan_commands.inc"
#undef NVN
#undef READ
#undef WRITE
