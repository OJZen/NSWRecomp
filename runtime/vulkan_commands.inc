// NVN commands remain data in guest command memory. Validation happens both
// when recording and when replaying. A submit completes its Vulkan fence before
// returning; GPU work is never claimed complete merely because it was recorded.
#include <math.h>
#include <time.h>
typedef struct {uint64_t type,address;} CounterReport;
typedef struct {uint64_t serial; uint32_t stages;} ProgramBinding;
typedef struct {tex_t texture; view_t view; bool has_view; NVNcopyRegion region; p_clear_color color;} TextureClear;
typedef struct {tex_t source,destination; view_t source_view,destination_view; bool has_source_view,has_destination_view; NVNcopyRegion source_region,destination_region; int flags;} TextureCopy;
typedef struct {
    int count;
    tex_t colors[8]; view_t views[8]; uint8_t has_view[8];
    tex_t depth; view_t depth_view; bool has_depth_view;
} RenderTargets;
typedef struct {
    unsigned origin,depth_mode;
    RenderTargets rt; p_scissor scissor; bool has_scissor;
    uint8_t draw_states[19][24]; float viewports[16][6]; p_depth_range depth_range;
    uint64_t scratch_address, scratch_size;
    dpool_t texture_pool, sampler_pool;
    uint64_t programs[6], textures[6][32], images[6][32];
    NVNbufferRange uniform[6][14], storage[6][16], vertex[16];
    uint32_t attributes[16]; vs_st streams[16];
    float blend_color[4]; int stencil[2][3]; p_poly_offset polygon_offset;
} CommandState;
static pthread_mutex_t submit_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {NVNqueue *queue; CommandState state;} queue_states[8];
static void command_forget(NVNqueue *q) {
    pthread_mutex_lock(&submit_lock);
    for(unsigned i=0;i<8;i++) if(queue_states[i].queue==q) {memset(&queue_states[i],0,sizeof queue_states[i]); break;}
    pthread_mutex_unlock(&submit_lock);
}
static void texture_copy(tex_t *out, const NVNtexture *texture) {
    *out=texture?( *texture_check(texture)):(tex_t){0};
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int count, const NVNtexture *const *colors,
    const NVNtextureView *const *views, const NVNtexture *depth, const NVNtextureView *depth_view) {
    if(count<0 || count>8) die("invalid NVN render target count");
    if(count) check(colors,(size_t)count*sizeof *colors,1);
    if(views) check(views,(size_t)count*sizeof *views,1);
    RenderTargets *r=command_record(c,OP_SET_RT,sizeof *r); r->count=count;
    for(int i=0;i<count;i++) {
        texture_copy(&r->colors[i],colors[i]);
        if(views && views[i]) {r->views[i]=*READ(view_t,views[i]); r->has_view[i]=1;}
    }
    texture_copy(&r->depth,depth);
    if(depth_view) {r->depth_view=*READ(view_t,depth_view); r->has_depth_view=true;}
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    if(index<0 || index>7 || (mask&~15)) die("invalid NVN clear target or mask"); check(rgba,16,1);
    p_clear_color *r=command_record(c,OP_CLEAR_COLOR,sizeof *r); r->index=index; r->mask=mask; memcpy(r->rgba,rgba,16);
    fprintf(stderr,"NVN recorded color clear target=%d mask=%d\n",index,mask);
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t mask, int stencil, int stencil_mask) {
    p_clear_ds *r=command_record(c,OP_CLEAR_DS,sizeof *r); *r=(p_clear_ds){depth,(int)(mask&255),stencil,stencil_mask};
}
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c,const NVNtexture *texture,const NVNtextureView *view,const NVNcopyRegion *region,const float *rgba,int mask) {
    const tex_t *t=texture_check(texture); check(region,sizeof *region,1); check(rgba,16,1);
    if(mask&~15) die("invalid NVN texture clear mask");
    TextureClear *r=command_record(c,OP_CLEAR_TEX,sizeof *r); r->texture=*t; r->region=*region; r->color.mask=mask;
    memcpy(r->color.rgba,rgba,16); if(view) {r->view=*READ(view_t,view); r->has_view=true;}
    fprintf(stderr,"NVN recorded texture clear %dx%dx%d format=%u mask=%d\n",region->width,region->height,region->depth,t->d.format,mask);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c,const NVNtexture *source,const NVNtextureView *source_view,const NVNcopyRegion *source_region,
    const NVNtexture *destination,const NVNtextureView *destination_view,const NVNcopyRegion *destination_region,int flags) {
    check(source_region,sizeof *source_region,1); check(destination_region,sizeof *destination_region,1);
    TextureCopy *r=command_record(c,OP_COPY_T2T,sizeof *r); r->source=*texture_check(source); r->destination=*texture_check(destination);
    r->source_region=*source_region; r->destination_region=*destination_region; r->flags=flags;
    if(source_view) {r->source_view=*READ(view_t,source_view); r->has_source_view=true;}
    if(destination_view) {r->destination_view=*READ(view_t,destination_view); r->has_destination_view=true;}
}
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c,int count,const NVNcommandHandle *handles) {
    if(count<0 || count>4096) die("invalid NVN call count"); if(count) check(handles,(size_t)count*8,1);
    for(int i=0;i<count;i++) {check((void *)(uintptr_t)handles[i],8,1); *(uint64_t *)command_record(c,OP_CALL,8)=handles[i];}
}
typedef struct {uint8_t *bytes; size_t size,capacity; unsigned budget;} CommandCopy;
static void snapshot_commands(CommandCopy *copy,uint64_t address,unsigned depth) {
    if(depth>16) die("NVN command copy nesting limit reached");
    for(;;) {
        if(!copy->budget--) die("NVN command copy traversal limit reached");
        check((void *)(uintptr_t)address,8,1); op_hdr header; memcpy(&header,(void *)(uintptr_t)address,8);
        if(!nvn_command_valid((void *)(uintptr_t)address,header) || header.size>(1u<<20)) die("invalid NVN copy command header");
        check((void *)(uintptr_t)address,header.size,1);
        if(header.op==OP_END) return;
        if(header.op==OP_CALL || header.op==OP_JUMP) {
            if(header.size<16) die("truncated NVN command copy branch"); uint64_t target; memcpy(&target,(void *)(uintptr_t)(address+8),8);
            if(header.op==OP_JUMP) {address=target; continue;} snapshot_commands(copy,target,depth+1);
        } else if(header.op!=OP_BEGIN) {
            if(header.size>(16u<<20)-copy->size) die("NVN copied command storage limit reached");
            if(copy->size+header.size>copy->capacity) {
                size_t capacity=(copy->size+header.size+65535)&~(size_t)65535; void *bytes=realloc(copy->bytes,capacity);
                if(!bytes) die("NVN command snapshot allocation failed"); copy->bytes=bytes; copy->capacity=capacity;
            }
            memcpy(copy->bytes+copy->size,(void *)(uintptr_t)address,header.size); copy->size+=header.size;
        }
        address+=header.size;
    }
}
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c,int count,const NVNcommandHandle *handles) {
    if(count<0 || count>4096) die("invalid NVN copy count"); if(count) check(handles,(size_t)count*8,1);
    CommandCopy copy={.budget=1000000};
    for(int i=0;i<count;i++) snapshot_commands(&copy,handles[i],0);
    // Snapshot first: guest callback allocation may reuse the source arena.
    for(size_t at=0;at<copy.size;) {
        op_hdr header; memcpy(&header,copy.bytes+at,8);
        memcpy(command_record(c,header.op,header.size-8),copy.bytes+at+8,header.size-8); at+=header.size;
    }
    free(copy.bytes);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int width, int height) {
    if(width<0 || height<0) die("invalid NVN scissor"); p_scissor *r=command_record(c,OP_SCISSOR,sizeof *r); *r=(p_scissor){x,y,width,height};
}
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int count, const int *rects) {
    if(first!=0 || count!=1) die("Vulkan backend requires one scissor"); check(rects,16,1);
    h_nvnCommandBufferSetScissor(c,rects[0],rects[1],rects[2],rects[3]);
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int count, const float *viewports) {
    if(first<0 || count<0 || first>16 || count>16-first) die("invalid NVN viewport count");
    if(count) check(viewports,(size_t)count*16,1);
    p_viewports *r=command_record(c,OP_VIEWPORTS,sizeof *r); r->first=first; r->n=count;
    for(int i=0;i<count;i++) memcpy(r->v[i],viewports+4*i,16);
}
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int width, int height) {
    if(width<0 || height<0) die("invalid NVN viewport");
    p_viewports *r=command_record(c,OP_VIEWPORTS,sizeof *r); r->n=1; r->v[0][0]=x; r->v[0][1]=y; r->v[0][2]=width; r->v[0][3]=height;
}
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float near, float far) {
    p_depth_range *r=command_record(c,OP_DEPTH_RANGE,sizeof *r); *r=(p_depth_range){near,far};
}
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c,const float *color) {
    check(color,16,1); memcpy(command_record(c,OP_BLEND_COLOR,sizeof(p_blend_color)),color,16);
}
static void record_stencil(NVNcommandBuffer *c,NVNface face,int value,int which) {
    if(face<NVN_FACE_FRONT || face>NVN_FACE_FRONT_AND_BACK) die("invalid NVN stencil face");
    p_stencil *r=command_record(c,OP_STENCIL,sizeof *r); r->face=face; r->which=which;
    if(which==0) r->value_mask=value; else if(which==1) r->mask=value; else r->ref=value;
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c,NVNface face,int value) {record_stencil(c,face,value,0);}
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c,NVNface face,int value) {record_stencil(c,face,value,1);}
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c,NVNface face,int value) {record_stencil(c,face,value,2);}
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c,float factor,float units,float clamp) {
    *(p_poly_offset *)command_record(c,OP_POLY_OFFSET,sizeof(p_poly_offset))=(p_poly_offset){factor,units,clamp};
}
// Dynamic rasterization state is retained for the deferred shader draw command.
// Full attachment clears do not consume these fields.
static void record_dynamic(NVNcommandBuffer *c,int kind,const void *data,size_t size) {
    p_bind_state *r=command_record(c,OP_BIND_STATE,sizeof *r); r->kind=kind; memcpy(r->bytes,data,size);
}
NVN(nvnCommandBufferSetDepthBounds, void, NVNcommandBuffer *c,uint32_t enabled,float near,float far) {
    struct {uint32_t enabled; float near,far;} value={enabled,near,far}; record_dynamic(c,6,&value,sizeof value);
}
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c,float value) {record_dynamic(c,7,&value,4);}
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c,float value) {record_dynamic(c,8,&value,4);}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c,float value) {record_dynamic(c,9,&value,4);}
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c,int value) {record_dynamic(c,10,&value,4);}
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c,uint32_t value) {record_dynamic(c,11,&value,4);}
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c,uint32_t value) {record_dynamic(c,12,&value,4);}
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c,uint32_t enabled,int index) {
    uint32_t value[]={enabled,(uint32_t)index}; record_dynamic(c,13,value,sizeof value);
}
NVN(nvnCommandBufferSetPatchSize, void, NVNcommandBuffer *c,int value) {record_dynamic(c,14,&value,4);}
NVN(nvnCommandBufferSetInnerTessellationLevels, void, NVNcommandBuffer *c,const float *values) {check(values,8,1); record_dynamic(c,15,values,8);}
NVN(nvnCommandBufferSetOuterTessellationLevels, void, NVNcommandBuffer *c,const float *values) {check(values,16,1); record_dynamic(c,16,values,16);}
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c,uint32_t value) {record_dynamic(c,17,&value,4);}
NVN(nvnCommandBufferSetConservativeRasterDilate, void, NVNcommandBuffer *c,float value) {record_dynamic(c,18,&value,4);}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int flags) {
    *(int *)command_record(c,OP_BARRIER,sizeof(int))=flags;
}
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c,const NVNtexturePool *pool) {
    *(dpool_t *)command_record(c,OP_SET_TEX_POOL,sizeof(dpool_t))=*descriptor_check(pool);
}
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c,const NVNsamplerPool *pool) {
    *(dpool_t *)command_record(c,OP_SET_SAMP_POOL,sizeof(dpool_t))=*descriptor_check(pool);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c,NVNcounterType type,NVNbufferAddress address) {
    if(type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP) die("Vulkan pipeline counters are not implemented");
    if(address&15) die("misaligned NVN counter destination"); check((void *)(uintptr_t)address,16,3);
    *(CounterReport *)command_record(c,OP_REPORT_COUNTER,sizeof(CounterReport))=(CounterReport){type,address};
}
static void binding_range(int stage,int first,int count,int limit) {
    if(stage<0 || stage>=6 || first<0 || count<0 || first>limit || count>limit-first) die("NVN shader binding outside bounds");
}
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c,const NVNprogram *program,int stages) {
    if(stages&~63) die("invalid NVN shader stage mask"); uint64_t serial=program?program_check(program)->serial:0;
    *(ProgramBinding *)command_record(c,OP_BIND_PROGRAM,sizeof(ProgramBinding))=(ProgramBinding){serial,(uint32_t)stages};
}
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c,int count,const NVNvertexAttribState *states) {
    binding_range(0,0,count,16); if(count) check(states,(size_t)count*sizeof *states,1);
    p_bind_vattr *r=command_record(c,OP_BIND_VATTR,sizeof *r); r->n=count;
    for(int i=0;i<count;i++) memcpy(&r->st[i],&states[i],sizeof(va_st));
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c,int count,const NVNvertexStreamState *states) {
    binding_range(0,0,count,16); if(count) check(states,(size_t)count*sizeof *states,1);
    p_bind_vstream *r=command_record(c,OP_BIND_VSTREAM,sizeof *r); r->n=count;
    for(int i=0;i<count;i++) memcpy(&r->st[i],&states[i],sizeof(vs_st));
}
static void record_buffer(NVNcommandBuffer *c,unsigned op,int stage,int index,uint64_t address,size_t size) {
    binding_range(stage,index,1,op==OP_BIND_UBO?14:16);
    if(size) check((void *)(uintptr_t)address,size,op==OP_BIND_SSBO?3:1);
    *(p_bind_buf *)command_record(c,op,sizeof(p_bind_buf))=(p_bind_buf){stage,index,address,size};
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c,NVNshaderStage stage,int index,NVNbufferAddress address,size_t size) {record_buffer(c,OP_BIND_UBO,stage,index,address,size);}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c,NVNshaderStage stage,int index,NVNbufferAddress address,size_t size) {record_buffer(c,OP_BIND_SSBO,stage,index,address,size);}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c,NVNshaderStage stage,int first,int count,const NVNbufferRange *ranges) {
    binding_range(stage,first,count,14); if(count) check(ranges,(size_t)count*sizeof *ranges,1);
    for(int i=0;i<count;i++) record_buffer(c,OP_BIND_UBO,stage,first+i,ranges[i].address,ranges[i].size);
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c,NVNshaderStage stage,int first,int count,const NVNbufferRange *ranges) {
    binding_range(stage,first,count,16); if(count) check(ranges,(size_t)count*sizeof *ranges,1);
    for(int i=0;i<count;i++) record_buffer(c,OP_BIND_SSBO,stage,first+i,ranges[i].address,ranges[i].size);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c,int first,int count,const NVNbufferRange *ranges) {
    binding_range(0,first,count,16); if(count) check(ranges,(size_t)count*sizeof *ranges,1);
    p_bind_vbuf *r=command_record(c,OP_BIND_VBUF,sizeof *r); r->first=first; r->n=count;
    for(int i=0;i<count;i++) {if(ranges[i].size) check((void *)(uintptr_t)ranges[i].address,ranges[i].size,1); r->r[i]=ranges[i];}
}
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c,int index,NVNbufferAddress address,size_t size) {
    binding_range(0,index,1,16); if(size) check((void *)(uintptr_t)address,size,1);
    p_bind_vbuf *r=command_record(c,OP_BIND_VBUF,sizeof *r); r->first=index; r->n=1; r->r[0]=(NVNbufferRange){address,size};
}
static void record_handles(NVNcommandBuffer *c,unsigned op,int stage,int first,int count,const uint64_t *handles) {
    binding_range(stage,first,count,32); p_bind_handles *r=command_record(c,op,sizeof *r);
    r->stage=stage; r->first=first; r->n=count; if(count) memcpy(r->h,handles,(size_t)count*8);
}
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c,NVNshaderStage stage,int first,int count,const NVNtextureHandle *handles) {
    binding_range(stage,first,count,32); if(count) check(handles,(size_t)count*8,1); record_handles(c,OP_BIND_TEX,stage,first,count,handles);
}
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c,NVNshaderStage stage,int index,NVNtextureHandle handle) {record_handles(c,OP_BIND_TEX,stage,index,1,&handle);}
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c,NVNshaderStage stage,int first,int count,const NVNimageHandle *handles) {
    binding_range(stage,first,count,32); if(count) check(handles,(size_t)count*8,1); record_handles(c,OP_BIND_IMG,stage,first,count,handles);
}
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c,NVNshaderStage stage,int index,NVNimageHandle handle) {record_handles(c,OP_BIND_IMG,stage,index,1,&handle);}
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c,NVNdrawPrimitive primitive,int first,int count,int base_instance,int instances) {
    if(first<0 || count<0 || base_instance<0 || instances<0) die("invalid NVN array draw extent");
    *(p_draw *)command_record(c,OP_DRAW,sizeof(p_draw))=(p_draw){primitive,first,count,base_instance,instances};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c,NVNdrawPrimitive primitive,int first,int count) {h_nvnCommandBufferDrawArraysInstanced(c,primitive,first,count,0,1);}
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,int count,NVNbufferAddress address,int base_vertex,int base_instance,int instances) {
    if(count<0 || base_instance<0 || instances<0 || (unsigned)type>2) die("invalid NVN indexed draw extent");
    if(count) check((void *)(uintptr_t)address,(size_t)count*(1u<<type),1);
    *(p_draw_idx *)command_record(c,OP_DRAW_IDX,sizeof(p_draw_idx))=(p_draw_idx){primitive,type,count,base_vertex,base_instance,instances,address};
}
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,int count,NVNbufferAddress address,int base_vertex) {h_nvnCommandBufferDrawElementsInstanced(c,primitive,type,count,address,base_vertex,0,1);}
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,int count,NVNbufferAddress address) {h_nvnCommandBufferDrawElementsInstanced(c,primitive,type,count,address,0,0,1);}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c,int x,int y,int z) {
    if(x<0 || y<0 || z<0) die("invalid NVN compute extent"); *(p_dispatch *)command_record(c,OP_DISPATCH,sizeof(p_dispatch))=(p_dispatch){x,y,z,0};
}
static void bind_draw_state(NVNcommandBuffer *c,int kind,const void *state,size_t size) {
    check(state,size,1); p_bind_state *r=command_record(c,OP_BIND_STATE,sizeof *r); r->kind=kind; memcpy(r->bytes,state,size);
}
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c,const NVNblendState *s) {bind_draw_state(c,0,s,sizeof(blend_st));}
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c,const NVNcolorState *s) {bind_draw_state(c,1,s,sizeof(color_st));}
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c,const NVNchannelMaskState *s) {bind_draw_state(c,2,s,sizeof(chmask_st));}
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c,const NVNmultisampleState *s) {bind_draw_state(c,3,s,sizeof(ms_st));}
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c,const NVNpolygonState *s) {bind_draw_state(c,4,s,sizeof(poly_st));}
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c,const NVNdepthStencilState *s) {bind_draw_state(c,5,s,sizeof(ds_st));}
NVN(nvnCommandBufferSetShaderScratchMemory, void, NVNcommandBuffer *c,const NVNmemoryPool *pool,ptrdiff_t offset,size_t size) {
    uint64_t address=(uintptr_t)pool_range(pool,offset,size,3);
    if((address&4095) || (size&131071)) die("invalid NVN shader scratch alignment");
    p_bind_buf *r=command_record(c,OP_BIND_SSBO,sizeof *r); *r=(p_bind_buf){-1,-1,address,size};
}
static void execute_color(CommandState *state, const p_clear_color *clear) {
    if(!clear->mask) return;
    if(clear->index<0 || clear->index>=state->rt.count || clear->mask!=15) die("Vulkan clear requires a full RGBA mask and bound color target");
    const tex_t *t=&state->rt.colors[clear->index];
    if(t->magic!=MAGIC('V','K','T','X') || !t->serial || t->d.target!=NVN_TEXTURE_TARGET_2D || t->d.depth!=1 || t->d.levels!=1 || t->d.samples>1 ||
        (t->d.format!=NVN_FORMAT_RGBA8 && t->d.format!=NVN_FORMAT_RGBA8_SRGB && t->d.format!=NVN_FORMAT_R11G11B10F && t->d.format!=NVN_FORMAT_R8)) die("unsupported Vulkan clear texture layout or format");
    const view_t *view=&state->rt.views[clear->index];
    if(state->rt.has_view[clear->index] && ((view->set_levels && (view->base_level || (view->nlevels!=1 && view->nlevels!=-1))) ||
        (view->set_layers && (view->min_layer || (view->nlayers!=1 && view->nlayers!=-1))) ||
        (view->set_format && view->format!=t->d.format) || (view->set_target && view->target!=t->d.target) ||
        (view->swizzle && view->swizzle!=texture_swizzle(NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_A)))) die("unsupported Vulkan clear view");
    if(state->has_scissor && (state->scissor.x>0 || state->scissor.y>0 ||
        (int64_t)state->scissor.x+state->scissor.w<t->d.width || (int64_t)state->scissor.y+state->scissor.h<t->d.height)) die("partial Vulkan scissor clear is not implemented");
    float rgba[4]; VulkanColorFormat format=t->d.format==NVN_FORMAT_R8?VULKAN_COLOR_R8:t->d.format==NVN_FORMAT_R11G11B10F?VULKAN_COLOR_RGB11F:
        t->d.format==NVN_FORMAT_RGBA8_SRGB?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8;
    for(unsigned i=0;i<4;i++) {
        if(!isfinite(clear->rgba[i])) die("non-finite NVN clear color");
        if(format==VULKAN_COLOR_RGB11F && i<3 && clear->rgba[i]>64512) die("NVN floating color exceeds supported finite range");
        rgba[i]=fmaxf(0,clear->rgba[i]); if(format!=VULKAN_COLOR_RGB11F || i==3) rgba[i]=fminf(1,rgba[i]);
    }
    if(t->d.width>4096 || t->d.height>4096) die("NVN clear exceeds Vulkan target bounds");
    unsigned bpp=vulkan_color_bytes(format); size_t bytes=(size_t)t->d.width*t->d.height*bpp; unsigned char *pixels=malloc(bytes);
    if(!pixels) die("NVN clear readback allocation failed"); char error[256];
    pthread_mutex_lock(&texture_lock); NativeTexture *slot=NULL;
    for(unsigned i=0;i<64;i++) if(native_textures[i].serial==t->serial) {slot=&native_textures[i]; break;}
    if(!slot) for(unsigned i=0;i<64;i++) if(!native_textures[i].serial) {slot=&native_textures[i]; break;}
    if(!slot) {pthread_mutex_unlock(&texture_lock); free(pixels); die("NVN native texture limit reached");}
    if(!slot->target) {slot->target=vulkan_runtime_target(t->d.width,t->d.height,format,error); if(slot->target) slot->serial=t->serial;}
    bool ok=slot->target && vulkan_runtime_clear(slot->target,rgba,pixels,bytes,error);
    pthread_mutex_unlock(&texture_lock);
    if(!ok) {free(pixels); die("NVN Vulkan clear: %s",error);}
    for(size_t i=0;i<bytes;i+=bpp) if(!vulkan_color_matches(format,pixels+i,rgba)) {free(pixels); die("NVN Vulkan color attachment readback mismatch");}
    free(pixels);
    fprintf(stderr,"NVN Vulkan color attachment clear completed: %ux%u format=%u pixels=%llu rgba=%.6g,%.6g,%.6g,%.6g\n",
        t->d.width,t->d.height,t->d.format,(unsigned long long)(bytes/bpp),rgba[0],rgba[1],rgba[2],rgba[3]);
}
static void execute_depth(CommandState *state,const p_clear_ds *clear) {
    if(!clear->depth_mask && !(clear->stencil_mask&255)) return;
    const tex_t *t=&state->rt.depth; const view_t *v=&state->rt.depth_view;
    if(t->magic!=MAGIC('V','K','T','X') || !t->serial || t->d.target!=NVN_TEXTURE_TARGET_2D || t->d.depth!=1 || t->d.levels!=1 || t->d.samples>1 ||
        t->d.format!=NVN_FORMAT_DEPTH24_STENCIL8 || t->d.width>4096 || t->d.height>4096) die("unsupported Vulkan depth/stencil layout or format");
    if(state->rt.has_depth_view && ((v->set_levels && (v->base_level || (v->nlevels!=1 && v->nlevels!=-1))) ||
        (v->set_layers && (v->min_layer || (v->nlayers!=1 && v->nlayers!=-1))) || (v->set_format && v->format!=t->d.format) ||
        (v->set_target && v->target!=t->d.target))) die("unsupported Vulkan depth/stencil view");
    if(state->has_scissor && (state->scissor.x>0 || state->scissor.y>0 || (int64_t)state->scissor.x+state->scissor.w<t->d.width ||
        (int64_t)state->scissor.y+state->scissor.h<t->d.height)) die("partial Vulkan depth/stencil scissor is unsupported");
    uint8_t stencil_mask=(uint8_t)clear->stencil_mask; if(stencil_mask && stencil_mask!=255) die("partial Vulkan stencil mask is unsupported");
    if(clear->depth_mask && !isfinite(clear->depth)) die("non-finite NVN depth clear");
    float depth=clear->depth_mask?fminf(1,fmaxf(0,clear->depth)):0;
    size_t bytes=(size_t)t->d.width*t->d.height*5; void *pixels=malloc(bytes); if(!pixels) die("NVN depth readback allocation failed"); char error[256];
    pthread_mutex_lock(&texture_lock); NativeTexture *slot=NULL;
    for(unsigned i=0;i<64;i++) if(native_textures[i].serial==t->serial) {slot=&native_textures[i]; break;}
    if(!slot) for(unsigned i=0;i<64;i++) if(!native_textures[i].serial) {slot=&native_textures[i]; break;}
    if(!slot) {pthread_mutex_unlock(&texture_lock); free(pixels); die("NVN native texture limit reached");}
    if(!slot->target) {slot->target=vulkan_runtime_depth_target(t->d.width,t->d.height,error); if(slot->target) slot->serial=t->serial;}
    bool ok=slot->target && vulkan_runtime_clear_depth(slot->target,depth,clear->depth_mask!=0,(uint8_t)clear->stencil,stencil_mask,pixels,bytes,error);
    pthread_mutex_unlock(&texture_lock); free(pixels); if(!ok) die("NVN Vulkan depth/stencil clear: %s",error);
    fprintf(stderr,"NVN Vulkan depth/stencil clear completed: %ux%u pixels=%llu depth=%g depth_mask=%d stencil=%u stencil_mask=%u\n",
        t->d.width,t->d.height,(unsigned long long)(bytes/5),depth,clear->depth_mask,(uint8_t)clear->stencil,stencil_mask);
}
#include "vulkan_shader_capture.inc"
#include "vulkan_shader.inc"
#include "vulkan_draw_commands.inc"
static void execute_texture_copy(const TextureCopy *copy) {
    const tex_t *textures[2]={&copy->source,&copy->destination};
    const NVNcopyRegion *regions[2]={&copy->source_region,&copy->destination_region};
    const view_t *views[2]={copy->has_source_view?&copy->source_view:NULL,copy->has_destination_view?&copy->destination_view:NULL};
    bool convert=textures[0]->d.format==NVN_FORMAT_R11G11B10F && (textures[1]->d.format==NVN_FORMAT_RGBA16F || textures[1]->d.format==NVN_FORMAT_RGBA8 || textures[1]->d.format==NVN_FORMAT_RGBA8_SRGB);
    if((copy->flags&~1) || textures[0]->serial==textures[1]->serial || (textures[0]->d.format!=textures[1]->d.format && !convert))
        die("unsupported Vulkan texture copy flags, alias or format conversion");
    for(unsigned i=0;i<2;i++) {
        const tex_t *t=textures[i]; const NVNcopyRegion *r=regions[i]; const view_t *v=views[i];
        if(t->magic!=MAGIC('V','K','T','X') || !t->serial || t->d.target!=NVN_TEXTURE_TARGET_2D || t->d.depth!=1 || t->d.samples>1 || t->d.levels!=1 ||
            r->xoffset || r->yoffset || r->zoffset || r->width<0 || r->height<0 || (unsigned)r->width!=t->d.width || (unsigned)r->height!=t->d.height || r->depth!=1 ||
            (v && ((v->set_levels && (v->base_level || (v->nlevels!=1 && v->nlevels!=-1))) || (v->set_layers && (v->min_layer || (v->nlayers!=1 && v->nlayers!=-1))) ||
             (v->set_format && v->format!=t->d.format) || (v->set_target && v->target!=t->d.target)))) die("unsupported Vulkan texture copy subresource");
    }
    bool depth=textures[0]->d.format==NVN_FORMAT_DEPTH24_STENCIL8;
    VulkanColorFormat format=textures[0]->d.format==NVN_FORMAT_R8?VULKAN_COLOR_R8:textures[0]->d.format==NVN_FORMAT_R11G11B10F?VULKAN_COLOR_RGB11F:
        textures[0]->d.format==NVN_FORMAT_RGBA8_SRGB?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8;
    if(!depth && textures[0]->d.format!=NVN_FORMAT_R8 && textures[0]->d.format!=NVN_FORMAT_R11G11B10F && textures[0]->d.format!=NVN_FORMAT_RGBA8_SRGB && textures[0]->d.format!=NVN_FORMAT_RGBA8)
        die("unsupported Vulkan texture copy format");
    VulkanColorFormat destination_format=convert?(textures[1]->d.format==NVN_FORMAT_RGBA16F?VULKAN_COLOR_RGBA16F:textures[1]->d.format==NVN_FORMAT_RGBA8_SRGB?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8):format;
    size_t count=(size_t)textures[1]->d.width*textures[1]->d.height,bytes=count*(depth?5:vulkan_color_bytes(destination_format));
    if(bytes>(64u<<20)) die("Vulkan copy readback exceeds bound");
    void *pixels=malloc(bytes); if(!pixels) die("Vulkan copy readback allocation failed");
    pthread_mutex_lock(&texture_lock); NativeTexture *source=NULL,*destination=NULL;
    for(unsigned i=0;i<64;i++) {
        if(native_textures[i].serial==textures[0]->serial) source=&native_textures[i];
        if(native_textures[i].serial==textures[1]->serial) destination=&native_textures[i];
    }
    if(!source || !source->target) {pthread_mutex_unlock(&texture_lock); free(pixels); die("Vulkan copy source needs texture upload");}
    if(!destination) for(unsigned i=0;i<64;i++) if(!native_textures[i].serial) {destination=&native_textures[i]; break;}
    if(!destination) {pthread_mutex_unlock(&texture_lock); free(pixels); die("Vulkan native texture limit reached");}
    char error[256];
    if(!destination->target) {
        destination->target=depth?vulkan_runtime_depth_target(textures[1]->d.width,textures[1]->d.height,error):vulkan_runtime_target(textures[1]->d.width,textures[1]->d.height,destination_format,error);
        if(destination->target) destination->serial=textures[1]->serial;
    }
    bool ok=destination->target && vulkan_runtime_copy(source->target,destination->target,(copy->flags&1)!=0,pixels,bytes,error);
    pthread_mutex_unlock(&texture_lock); free(pixels);
    if(!ok) die("NVN Vulkan texture copy: %s",error);
    fprintf(stderr,"NVN Vulkan texture copy completed: %ux%u format=%u verified_pixels=%llu\n",textures[0]->d.width,textures[0]->d.height,textures[0]->d.format,(unsigned long long)count);
}
static void execute_commands(uint64_t address, CommandState *state, unsigned depth, unsigned *budget) {
    if(depth>16) die("NVN nested command limit reached");
    for(;;) {
        if(!(*budget)--) die("NVN command replay limit reached");
        check((void *)(uintptr_t)address,8,1); op_hdr header; memcpy(&header,(void *)(uintptr_t)address,8);
        if(!nvn_command_valid((void *)(uintptr_t)address,header) || header.size>(1u<<20)) die("invalid NVN command header");
        check((void *)(uintptr_t)address,header.size,1); const void *p=(void *)(uintptr_t)(address+8);
#define PAYLOAD(T) do {if(header.size<8+sizeof(T)) die("truncated NVN command payload");} while(0)
        switch(header.op) {
        case OP_BEGIN: break;
        case OP_END: return;
        case OP_JUMP: PAYLOAD(uint64_t); memcpy(&address,p,8); continue;
        case OP_CALL: {PAYLOAD(uint64_t); uint64_t nested; memcpy(&nested,p,8); execute_commands(nested,state,depth+1,budget); break;}
        case OP_SET_RT: PAYLOAD(RenderTargets); memcpy(&state->rt,p,sizeof state->rt); if(state->rt.count<0 || state->rt.count>8) die("invalid NVN bound targets"); break;
        case OP_SCISSOR: PAYLOAD(p_scissor); memcpy(&state->scissor,p,sizeof state->scissor); state->has_scissor=true; break;
        case OP_CLEAR_COLOR: PAYLOAD(p_clear_color); execute_color(state,p); break;
        case OP_CLEAR_DS: PAYLOAD(p_clear_ds); execute_depth(state,p); break;
        case OP_CLEAR_TEX: {PAYLOAD(TextureClear); const TextureClear *r=p;
            if(r->region.xoffset || r->region.yoffset || r->region.zoffset || r->region.width!=(int)r->texture.d.width ||
                r->region.height!=(int)r->texture.d.height || r->region.depth!=1) die("partial Vulkan texture clear is not implemented");
            CommandState clear_state={0}; clear_state.rt.count=1; clear_state.rt.colors[0]=r->texture;
            clear_state.rt.views[0]=r->view; clear_state.rt.has_view[0]=r->has_view;
            execute_color(&clear_state,&r->color); break;}
        // Viewport/depth range affect rasterization, not full attachment clears.
        // Shader draws remain an explicit unsupported operation in this backend.
        case OP_VIEWPORTS: {PAYLOAD(p_viewports); const p_viewports *v=p;
            if(v->first<0 || v->first>16 || v->n<0 || v->n>16-v->first) die("invalid NVN recorded viewports");
            for(int i=0;i<v->n;i++) memcpy(state->viewports[v->first+i],v->v[i],16); break;}
        case OP_DEPTH_RANGE: PAYLOAD(p_depth_range); memcpy(&state->depth_range,p,sizeof state->depth_range); break;
        case OP_BLEND_COLOR: PAYLOAD(p_blend_color); memcpy(state->blend_color,p,sizeof state->blend_color); break;
        case OP_POLY_OFFSET: PAYLOAD(p_poly_offset); memcpy(&state->polygon_offset,p,sizeof state->polygon_offset); break;
        case OP_STENCIL: {PAYLOAD(p_stencil); const p_stencil *s=p;
            if(s->face<NVN_FACE_FRONT || s->face>NVN_FACE_FRONT_AND_BACK || s->which<0 || s->which>2) die("invalid recorded stencil state");
            int value=s->which==0?s->value_mask:s->which==1?s->mask:s->ref;
            for(unsigned i=0;i<2;i++) if(s->face&(1u<<i)) state->stencil[i][s->which]=value; break;}
        case OP_BIND_STATE: {PAYLOAD(p_bind_state); const p_bind_state *s=p;
            if(s->kind<0 || s->kind>=19) die("invalid NVN draw state kind"); memcpy(state->draw_states[s->kind],s->bytes,sizeof s->bytes); break;}
        case OP_BIND_UBO: case OP_BIND_SSBO: {PAYLOAD(p_bind_buf); const p_bind_buf *s=p;
            if(header.op==OP_BIND_SSBO && s->stage==-1 && s->index==-1) {
                check((void *)(uintptr_t)s->addr,s->size,3); state->scratch_address=s->addr; state->scratch_size=s->size; break;
            }
            binding_range(s->stage,s->index,1,header.op==OP_BIND_UBO?14:16);
            NVNbufferRange *slot=header.op==OP_BIND_UBO?&state->uniform[s->stage][s->index]:&state->storage[s->stage][s->index];
            *slot=(NVNbufferRange){s->addr,s->size}; break;}
        case OP_BIND_PROGRAM: {PAYLOAD(ProgramBinding); const ProgramBinding *s=p; if(s->stages&~63u) die("invalid recorded stage mask");
            for(unsigned i=0;i<6;i++) if(s->stages&(1u<<i)) state->programs[i]=s->serial; break;}
        case OP_BIND_VATTR: {PAYLOAD(p_bind_vattr); const p_bind_vattr *s=p; binding_range(0,s->first,s->n,16); memcpy(state->attributes+s->first,s->st,(size_t)s->n*sizeof *s->st); break;}
        case OP_BIND_VSTREAM: {PAYLOAD(p_bind_vstream); const p_bind_vstream *s=p; binding_range(0,s->first,s->n,16); memcpy(state->streams+s->first,s->st,(size_t)s->n*sizeof *s->st); break;}
        case OP_BIND_VBUF: {PAYLOAD(p_bind_vbuf); const p_bind_vbuf *s=p; binding_range(0,s->first,s->n,16); memcpy(state->vertex+s->first,s->r,(size_t)s->n*sizeof *s->r); break;}
        case OP_BIND_TEX: case OP_BIND_IMG: {PAYLOAD(p_bind_handles); const p_bind_handles *s=p; binding_range(s->stage,s->first,s->n,32);
            if(s->deferred) die("deferred Vulkan handles are unsupported");
            uint64_t *dest=header.op==OP_BIND_TEX?state->textures[s->stage]:state->images[s->stage]; memcpy(dest+s->first,s->h,(size_t)s->n*8); break;}
        case OP_DRAW: PAYLOAD(p_draw); execute_draw(state,p,NULL); break;
        case OP_DRAW_IDX: PAYLOAD(p_draw_idx); execute_draw(state,NULL,p); break;
        case OP_DISPATCH: PAYLOAD(p_dispatch); execute_compute(state,p); break;
        case OP_COPY_T2T: {PAYLOAD(TextureCopy); const TextureCopy *copy=p;
            fprintf(stderr,"NVN pending texture copy: source=%llu %ux%u format=%u levels=%u target=%u destination=%llu %ux%u format=%u levels=%u target=%u flags=%x region=%d,%d,%d %dx%dx%d to %d,%d,%d %dx%dx%d\n",
                (unsigned long long)copy->source.serial,copy->source.d.width,copy->source.d.height,copy->source.d.format,copy->source.d.levels,copy->source.d.target,
                (unsigned long long)copy->destination.serial,copy->destination.d.width,copy->destination.d.height,copy->destination.d.format,copy->destination.d.levels,copy->destination.d.target,copy->flags,
                copy->source_region.xoffset,copy->source_region.yoffset,copy->source_region.zoffset,copy->source_region.width,copy->source_region.height,copy->source_region.depth,
                copy->destination_region.xoffset,copy->destination_region.yoffset,copy->destination_region.zoffset,copy->destination_region.width,copy->destination_region.height,copy->destination_region.depth);
            execute_texture_copy(copy); break;}
        case OP_BARRIER: PAYLOAD(int); break; // Every implemented operation waits for its GPU fence.
        case OP_SET_TEX_POOL: PAYLOAD(dpool_t); memcpy(&state->texture_pool,p,sizeof(dpool_t)); break;
        case OP_SET_SAMP_POOL: PAYLOAD(dpool_t); memcpy(&state->sampler_pool,p,sizeof(dpool_t)); break;
        case OP_REPORT_COUNTER: {PAYLOAD(CounterReport); const CounterReport *r=p;
            if((r->type!=NVN_COUNTER_TYPE_TIMESTAMP && r->type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP) || (r->address&15)) die("invalid recorded NVN timestamp");
            check((void *)(uintptr_t)r->address,16,3); struct timespec now;
            if(clock_gettime(CLOCK_MONOTONIC,&now)) die("NVN host completion clock failed");
            // Synchronous queue completion clock, in the NVN 614.4 MHz unit.
            // This is a host timeline, not a hardware GPU profiling query.
            uint64_t value[2]={0,(uint64_t)now.tv_sec*614400000+(uint64_t)now.tv_nsec*384/625};
            memcpy((void *)(uintptr_t)r->address,value,sizeof value); break;}
        default: die("unimplemented Vulkan NVN command %u",header.op);
        }
#undef PAYLOAD
        address+=header.size;
    }
}
NVN(nvnQueueSubmitCommands, void, NVNqueue *queue, int count, const NVNcommandHandle *handles) {
    queue_t *q=queue_check(queue); if(count<0 || count>4096) die("invalid NVN submit count");
    if(count) check(handles,count*sizeof *handles,1);
    pthread_mutex_lock(&submit_lock); unsigned budget=1000000;
    CommandState *state=NULL;
    for(unsigned i=0;i<8;i++) if(queue_states[i].queue==queue) {state=&queue_states[i].state; break;}
    if(!state) for(unsigned i=0;i<8;i++) if(!queue_states[i].queue) {
        queue_states[i].queue=queue; state=&queue_states[i].state; state->depth_range=(p_depth_range){0,1};
        uint32_t sample_mask=UINT32_MAX; memcpy(state->draw_states[10],&sample_mask,4); break;
    }
    if(!state) {pthread_mutex_unlock(&submit_lock); die("NVN queue replay context limit reached");}
    const dev_t_ *device=READ(dev_t_,q->dev); state->origin=device->origin; state->depth_mode=device->depth_mode;
    for(int i=0;i<count;i++) execute_commands(handles[i],state,0,&budget);
    q->submits++; pthread_mutex_unlock(&submit_lock);
}
NVN(nvnQueuePresentTexture, void, NVNqueue *queue,NVNwindow *window,int index) {
    queue_check(queue); win_t *w=window_check(window);
    if(index<0 || index>=w->ntex || !w->ticket[index]) die("NVN offscreen presentation requires an acquired texture");
    const tex_t *texture=texture_check(w->tex[index]);
    if(texture->d.format!=NVN_FORMAT_RGBA8 && texture->d.format!=NVN_FORMAT_RGBA8_SRGB) die("unsupported offscreen presentation format");
    size_t bytes=(size_t)texture->d.width*texture->d.height*4; if(bytes>(64u<<20)) die("offscreen presentation size bound");
    uint8_t *pixels=malloc(bytes); if(!pixels) die("offscreen presentation allocation failed"); char error[256];
    pthread_mutex_lock(&submit_lock); pthread_mutex_lock(&texture_lock);
    VulkanTarget *native=draw_native(texture)->target; bool ok=vulkan_runtime_pixels(native,false,pixels,bytes,error);
    pthread_mutex_unlock(&texture_lock);
    if(ok) {w->ticket[index]=0; w->presents++;}
    pthread_mutex_unlock(&submit_lock);
    if(!ok) {free(pixels); die("NVN offscreen presentation: %s",error);}
    bool queued = false;
    if(!vulkan_display_frame(pixels,bytes,texture->d.width,texture->d.height,&queued,error)) {
        free(pixels); die("NVN Windows preview: %s",error);
    }
    size_t varying=0; for(size_t i=4;i<bytes;i+=4) varying+=memcmp(pixels,pixels+i,4)!=0;
    const char *capture=getenv("NSW_VULKAN_CAPTURE");
    if(capture && !strcmp(capture,"1") && w->presents<=8) {char name[64]; snprintf(name,sizeof name,"frame-%llu.rgba",(unsigned long long)w->presents); shader_capture_file(name,pixels,bytes);}
    if(capture && !strcmp(capture,"1") && varying) {
        static bool captured_visible;
        size_t color_varying=0;
        for(size_t i=4;i<bytes;i+=4) color_varying+=memcmp(pixels,pixels+i,3)!=0;
        if(color_varying) {
            char metadata[256]; int length=snprintf(metadata,sizeof metadata,"{\"frame\":%llu,\"width\":%u,\"height\":%u,\"varying_color_pixels\":%llu,\"window_queued\":%s}\n",
                (unsigned long long)w->presents,texture->d.width,texture->d.height,(unsigned long long)color_varying,queued?"true":"false");
            if(length<0 || (size_t)length>=sizeof metadata) die("frame metadata exceeds bound");
            if(!captured_visible) {
                shader_capture_file("first-visible-frame.rgba",pixels,bytes); shader_capture_file("first-visible-frame.json",metadata,(size_t)length); captured_visible=true;
            }
            // One rolling slot permits inspection after a dark initial fade without
            // retaining an unbounded sequence of game-derived frames.
            shader_capture_file("latest-frame.rgba",pixels,bytes); shader_capture_file("latest-frame.json",metadata,(size_t)length);
        }
    }
    free(pixels);
    fprintf(stderr,"NVN readback frame completed: frame=%llu %ux%u varying_pixels=%llu window_queued=%s\n",(unsigned long long)w->presents,texture->d.width,texture->d.height,(unsigned long long)varying,queued?"true":"false");
}
