// Vulkan framebuffer rows start at the top. NVN lower-left viewports need
// a negative height; upper-left viewports already use Vulkan's row direction.
static void draw_coordinates(const CommandState *state,unsigned height,VulkanDraw *draw) {
    const float *v=state->viewports[0]; for(unsigned i=0;i<4;i++) if(!isfinite(v[i])) die("non-finite Vulkan viewport");
    draw->viewport=(VkViewport){v[0],v[1],v[2],v[3],state->depth_range.n,state->depth_range.f};
    if(state->origin==0) {draw->viewport.y=height-v[1]; draw->viewport.height=-v[3];}
    if(v[2]<=0 || v[3]<=0 || state->scissor.x<0 || state->scissor.y<0 || state->scissor.w<0 || state->scissor.h<0) die("unsupported Vulkan viewport or scissor extent");
    draw->scissor=(VkRect2D){{state->scissor.x,state->scissor.y},{state->scissor.w,state->scissor.h}};
    if(state->origin==0) draw->scissor.offset.y=height-state->scissor.y-state->scissor.h;
}
#include "vulkan_shader_checks.h"
static const ProgramRecord *draw_program(uint64_t serial,unsigned stage,const void **source,size_t *size) {
    for(unsigned i=0;i<256;i++) if(programs[i].serial==serial) for(unsigned j=0;j<programs[i].count;j++) if(program_word(programs[i].blocks[j],0x714)==stage) {
        *source=programs[i].blocks[j]; *size=programs[i].lengths[j]; return &programs[i];
    }
    die("draw shader stage %u is unbound",stage);
}
static uint8_t *draw_packet(CommandState *state,unsigned operation,const void *command,size_t command_size) {
    pthread_mutex_lock(&program_lock); const void *sources[2]; size_t sizes[2];
    for(unsigned i=0;i<2;i++) draw_program(state->programs[i],i,&sources[i],&sizes[i]);
    char path[160]; snprintf(path,sizeof path,"work/vulkan-shaders/cache/draw-%016llx-%016llx-%u-%u.vkdraw",
        (unsigned long long)shader_index(sources[0],sizes[0]),(unsigned long long)shader_index(sources[1],sizes[1]),state->origin,state->depth_mode);
    FILE *file=fopen(path,"rb");
    if(!file) {pthread_mutex_unlock(&program_lock); shader_capture(state,operation,command,command_size); die("Vulkan draw packet missing; prepare captured shader pair");}
    uint32_t header[12];
    if(fread(header,1,sizeof header,file)!=sizeof header || header[0]!=0x4457534e || header[1]!=1 || header[2]!=sizes[0] || header[3]!=sizes[1] ||
        header[4]<20 || header[4]>(4u<<20) || header[5]<20 || header[5]>(4u<<20) || ((header[2]|header[3]|header[4]|header[5])&3) ||
        !header[6] || header[6]>32 || header[7]!=state->origin || header[8]!=state->depth_mode || header[9] || header[10] || header[11]) die("invalid Vulkan draw packet header");
    size_t bytes=48+sizes[0]+sizes[1]+header[6]*20+header[4]+header[5]; uint8_t *packet=malloc(bytes); if(!packet) die("draw packet allocation failed");
    memcpy(packet,header,48); bool ok=fread(packet+48,1,bytes-48,file)==bytes-48 && fgetc(file)==EOF && !ferror(file) &&
        !memcmp(packet+48,sources[0],sizes[0]) && !memcmp(packet+48+sizes[0],sources[1],sizes[1]);
    int closed=fclose(file); pthread_mutex_unlock(&program_lock);
    if(!ok || closed) {free(packet); die("Vulkan draw packet source mismatch");} return packet;
}
static Descriptor draw_descriptor(const dpool_t *pool,unsigned index,unsigned kind) {
    if(index<256 || index>=(unsigned)pool->n || !pool->pool) die("draw descriptor outside bound pool");
    uint64_t address=(uintptr_t)pool_range((const NVNmemoryPool *)pool->pool,pool->off+(size_t)index*32,32,1);
    Descriptor value={0}; pthread_mutex_lock(&descriptor_lock);
    for(unsigned i=0;i<4096;i++) if(descriptors[i].address==address) {value=descriptors[i]; break;}
    pthread_mutex_unlock(&descriptor_lock); if(value.kind!=kind) die("draw references an unregistered descriptor"); return value;
}
static VkSamplerAddressMode draw_wrap(unsigned mode) {
    switch(mode) {case NVN_WRAP_MODE_REPEAT:return VK_SAMPLER_ADDRESS_MODE_REPEAT; case NVN_WRAP_MODE_MIRRORED_REPEAT:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case NVN_WRAP_MODE_CLAMP_TO_EDGE:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case NVN_WRAP_MODE_CLAMP_TO_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:die("unsupported Vulkan sampler wrap %u",mode);}
}
static NativeTexture *draw_native(const tex_t *texture) {
    for(unsigned i=0;i<64;i++) if(native_textures[i].serial==texture->serial && native_textures[i].target) return &native_textures[i];
    if(texture->magic!=MAGIC('V','K','T','X') || !texture->serial || texture->d.target!=NVN_TEXTURE_TARGET_2D || texture->d.levels!=1 || texture->d.samples>1 || texture->d.depth!=1)
        die("unsupported Vulkan texture upload layout");
    bool depth=texture->d.format==NVN_FORMAT_DEPTH24_STENCIL8; VulkanColorFormat format;
    switch(texture->d.format) {
        case NVN_FORMAT_R8:format=VULKAN_COLOR_R8;break;case NVN_FORMAT_RGBA8:format=VULKAN_COLOR_RGBA8;break;case NVN_FORMAT_RGBA8_SRGB:format=VULKAN_COLOR_SRGBA8;break;
        case NVN_FORMAT_R11G11B10F:format=VULKAN_COLOR_RGB11F;break;case NVN_FORMAT_RGBA16F:format=VULKAN_COLOR_RGBA16F;break;case NVN_FORMAT_DEPTH24_STENCIL8:format=VULKAN_COLOR_RGBA8;break;
        default:die("unsupported Vulkan texture upload format %u",texture->d.format);
    }
    size_t count=(size_t)texture->d.width*texture->d.height; unsigned bpp=depth?4:vulkan_color_bytes(format); size_t packed_bytes=count*bpp,bytes=depth?count*5:packed_bytes;
    if(!bytes || bytes>(64u<<20) || texture->L.size>(128u<<20)) die("Vulkan texture upload size bound");
    pool_range((const NVNmemoryPool *)texture->pool,texture->off,texture->L.size,1);
    uint8_t *packed=malloc(bytes); if(!packed) die("Vulkan upload allocation failed");
    if(texture->d.flags&0x50) {
        size_t stride=texture->L.size/texture->d.height; if(stride<(size_t)texture->d.width*bpp) die("invalid linear texture stride");
        for(unsigned y=0;y<texture->d.height;y++) memcpy(packed+(size_t)y*texture->d.width*bpp,texture->mem+y*stride,(size_t)texture->d.width*bpp);
    } else if(nvn_deswizzle(&texture->d,0,0,texture->mem,packed,(size_t)texture->d.width*bpp)) die("Vulkan texture deswizzle failed");
    if(depth) for(size_t i=0;i<count;i++) {packed[packed_bytes+i]=packed[i*4+3]; packed[i*4+3]=0;}
    NativeTexture *slot=NULL; for(unsigned i=0;i<64;i++) if(!native_textures[i].serial) {slot=&native_textures[i]; break;} if(!slot) die("Vulkan native texture limit reached");
    char error[256]; slot->target=depth?vulkan_runtime_depth_target(texture->d.width,texture->d.height,error):vulkan_runtime_target(texture->d.width,texture->d.height,format,error);
    bool ok=slot->target && vulkan_runtime_pixels(slot->target,true,packed,bytes,error); free(packed);
    if(!ok) die("NVN Vulkan texture upload: %s",error); slot->serial=texture->serial; return slot;
}
static bool draw_full_view(const tex_t *t,const view_t *v) {
    return (!v->set_levels || (!v->base_level && (v->nlevels==1 || v->nlevels==-1))) &&
        (!v->set_layers || (!v->min_layer && (v->nlayers==1 || v->nlayers==-1))) &&
        (!v->set_format || v->format==t->d.format) && (!v->set_target || v->target==t->d.target) &&
        (!v->swizzle || v->swizzle==texture_swizzle(NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_A));
}
static VkBlendFactor draw_blend_factor(unsigned factor) {
    switch(factor) {
        case NVN_BLEND_FUNC_ZERO:return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE:return VK_BLEND_FACTOR_ONE;
        case NVN_BLEND_FUNC_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case NVN_BLEND_FUNC_DST_COLOR:return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NVN_BLEND_FUNC_DST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case NVN_BLEND_FUNC_CONSTANT_COLOR:return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_CONSTANT_ALPHA:return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default:die("unsupported Vulkan blend factor %u",factor);
    }
}
static void execute_draw(CommandState *state,const p_draw *plain,const p_draw_idx *indexed) {
    unsigned operation=indexed?OP_DRAW_IDX:OP_DRAW; const void *command=indexed?(const void *)indexed:(const void *)plain;
    size_t command_size=indexed?sizeof *indexed:sizeof *plain;
    uint8_t *packet=draw_packet(state,operation,command,command_size); const uint32_t *header=(const uint32_t *)packet;
    const uint8_t *sources[2]={packet+48,packet+48+header[2]}; const uint32_t *records=(const uint32_t *)(sources[1]+header[3]);
    const uint32_t *vertex_spirv=records+header[6]*5; uint32_t vertex_locations; char location_error[256];
    if(!vulkan_vertex_locations(vertex_spirv,header[4],&vertex_locations,location_error)) die("NVN Vulkan vertex inputs: %s",location_error);
    if(state->rt.count!=1) die("Vulkan draw currently requires one color target");
    const tex_t *target=&state->rt.colors[0];
    if((state->rt.has_view[0] && !draw_full_view(target,&state->rt.views[0])) || (state->rt.has_depth_view && !draw_full_view(&state->rt.depth,&state->rt.depth_view)) ||
        target->d.target!=NVN_TEXTURE_TARGET_2D || target->d.levels!=1 || target->d.samples>1) die("unsupported Vulkan draw attachment view");
    VulkanDraw draw={0};
    ds_st ds; memcpy(&ds,state->draw_states[5],sizeof ds);
    if(state->rt.depth.serial) {
        if(ds.dw || (ds.st && (ds.fail_f!=1 || ds.zfail_f!=1 || ds.zpass_f!=1 || ds.fail_b!=1 || ds.zfail_b!=1 || ds.zpass_b!=1))) die("Vulkan draw depth/stencil writes are not implemented");
        if(ds.df<1 || ds.df>8 || ds.sf_f<1 || ds.sf_f>8 || ds.sf_b<1 || ds.sf_b>8) die("invalid Vulkan depth/stencil comparison");
        pthread_mutex_lock(&texture_lock); draw.depth=draw_native(&state->rt.depth)->target; pthread_mutex_unlock(&texture_lock);
        draw.depth_state=(VkPipelineDepthStencilStateCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=ds.dt,.depthCompareOp=ds.df-1,.stencilTestEnable=ds.st,
            .front={.compareOp=ds.sf_f-1,.compareMask=state->stencil[0][0],.writeMask=0,.reference=state->stencil[0][2]},
            .back={.compareOp=ds.sf_b-1,.compareMask=state->stencil[1][0],.writeMask=0,.reference=state->stencil[1][2]}};
    }
    int primitive=indexed?indexed->prim:plain->prim;
    draw.topology=primitive==NVN_DRAW_PRIMITIVE_TRIANGLES?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:primitive==NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    if(primitive!=4 && primitive!=5 && primitive!=6) die("unsupported Vulkan draw topology");
    draw.count=indexed?indexed->count:plain->count; draw.instances=indexed?indexed->instances:plain->instances; draw.base_instance=indexed?indexed->base_instance:plain->base_instance;
    draw.base_vertex=indexed?indexed->base_vertex:0; draw.first=plain?plain->first:0;
    if(!draw.count || !draw.instances) {free(packet); return;}
    uint32_t maximum=0;
    if(indexed) {
        if(indexed->itype!=NVN_INDEX_TYPE_UNSIGNED_SHORT && indexed->itype!=NVN_INDEX_TYPE_UNSIGNED_INT) die("unsupported Vulkan index type");
        unsigned width=indexed->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4; draw.index_type=width==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;
        if(draw.count>(1u<<20)) die("Vulkan index count exceeds bound");
        draw.indices=(void *)(uintptr_t)indexed->ibuf; draw.index_bytes=(size_t)draw.count*width; check(draw.indices,draw.index_bytes,1);
        for(unsigned i=0;i<draw.count;i++) {uint32_t value=0; memcpy(&value,(const uint8_t *)draw.indices+i*width,width); int64_t vertex=(int64_t)value+draw.base_vertex;
            if(vertex<0 || vertex>UINT32_MAX) die("Vulkan vertex index overflow"); if((uint32_t)vertex>maximum) maximum=(uint32_t)vertex;}
    } else {if(draw.count>UINT32_MAX-draw.first) die("Vulkan vertex extent overflow"); maximum=draw.first+draw.count-1;}
    for(unsigned i=0;i<16;i++) {
        if(!(vertex_locations&(1u<<i))) continue;
        va_st attribute; memcpy(&attribute,&state->attributes[i],sizeof attribute);
        if(!attribute.fmt) die("Vulkan shader vertex input %u is unbound",i);
        VkFormat format; unsigned bytes;
        switch(attribute.fmt) {case NVN_FORMAT_R32F:format=VK_FORMAT_R32_SFLOAT;bytes=4;break;case NVN_FORMAT_RG32F:format=VK_FORMAT_R32G32_SFLOAT;bytes=8;break;
            case NVN_FORMAT_RGB32F:format=VK_FORMAT_R32G32B32_SFLOAT;bytes=12;break;case NVN_FORMAT_RGBA32F:format=VK_FORMAT_R32G32B32A32_SFLOAT;bytes=16;break;default:die("unsupported Vulkan vertex format %u",attribute.fmt);}
        unsigned stream=attribute.stream; if(stream>=16 || state->streams[stream].divisor) die("unsupported Vulkan vertex stream divisor");
        NVNbufferRange range=state->vertex[stream]; uint64_t needed=(uint64_t)maximum*state->streams[stream].stride+attribute.offset+bytes;
        if(!range.address || needed>range.size || needed>(16u<<20)) die("Vulkan vertex fetch outside bound buffer"); check((void *)(uintptr_t)range.address,needed,1);
        if(draw.vertices[stream].size<needed) draw.vertices[stream]=(VulkanDrawVertex){(void *)(uintptr_t)range.address,needed,state->streams[stream].stride};
        draw.attributes[draw.attribute_count++]=(VkVertexInputAttributeDescription){i,stream,format,attribute.offset};
    }
    poly_st polygon; color_st color; chmask_st mask; ms_st multisample;
    memcpy(&polygon,state->draw_states[4],sizeof polygon); memcpy(&color,state->draw_states[1],sizeof color); memcpy(&mask,state->draw_states[2],sizeof mask); memcpy(&multisample,state->draw_states[3],sizeof multisample);
    if(polygon.mode!=NVN_POLYGON_MODE_FILL || polygon.offset_en || (color.blend_en&~1u) || color.logic_op!=NVN_LOGIC_OP_COPY || color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS ||
        multisample.samples>1 || multisample.a2c || program_word(state->draw_states[6],0) || program_word(state->draw_states[11],0) || program_word(state->draw_states[13],0) || program_word(state->draw_states[17],0))
        die("unsupported Vulkan draw raster or blend state");
    draw.cull=polygon.cull; draw.front=(polygon.front!=(state->origin==0))?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;
    draw.depth_clamp=program_word(state->draw_states[12],0)!=0; draw.sample_mask=program_word(state->draw_states[10],0);
    draw.blend=(VkPipelineColorBlendAttachmentState){.colorWriteMask=mask.mask&15};
    if(color.blend_en&1) {
        blend_st blend; memcpy(&blend,state->draw_states[0],sizeof blend);
        if(blend.target || blend.eq_rgb<1 || blend.eq_rgb>5 || blend.eq_a<1 || blend.eq_a>5) die("unsupported Vulkan blend target or equation");
        draw.blend.blendEnable=VK_TRUE; draw.blend.srcColorBlendFactor=draw_blend_factor(blend.src_rgb); draw.blend.dstColorBlendFactor=draw_blend_factor(blend.dst_rgb);
        draw.blend.srcAlphaBlendFactor=draw_blend_factor(blend.src_a); draw.blend.dstAlphaBlendFactor=draw_blend_factor(blend.dst_a);
        draw.blend.colorBlendOp=blend.eq_rgb-1; draw.blend.alphaBlendOp=blend.eq_a-1;
        memcpy(draw.blend_constants,state->blend_color,sizeof draw.blend_constants);
    }
    draw_coordinates(state,target->d.height,&draw);
    VulkanDrawUniform uniforms[32]={0}; VulkanDrawTexture textures[16]={0};
    draw.uniforms=uniforms; draw.textures=textures;
    for(unsigned i=0;i<header[6];i++) {
        const uint32_t *rec=records+i*5; unsigned type=rec[0],stage=rec[1],binding=rec[2],slot=rec[3];
        if(type>2 || stage>1 || rec[4]) die("invalid Vulkan draw binding record");
        if(type==1) {
            if(slot>=32 || draw.texture_count>=16) die("Vulkan draw sampler binding limit");
            uint64_t handle=state->textures[stage][slot]; Descriptor texture=draw_descriptor(&state->texture_pool,(unsigned)(handle&0xfffff),1);
            Descriptor sampler=draw_descriptor(&state->sampler_pool,(unsigned)(handle>>20),3); samp_par s=sampler.sampler;
            if(texture.texture.d.target!=NVN_TEXTURE_TARGET_2D || texture.texture.d.levels!=1 || !draw_full_view(&texture.texture,&texture.view) ||
                s.aniso>1 || s.cmp_mode || s.minf>5 || s.magf>1 || !isfinite(s.lod_bias) || s.lod_bias!=0) die("unsupported Vulkan combined sampler or texture view");
            VkSamplerCreateInfo info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=s.magf?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.minFilter=(s.minf&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
                .mipmapMode=s.minf>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=draw_wrap(s.ws),.addressModeV=draw_wrap(s.wt),.addressModeW=draw_wrap(s.wr),
                .minLod=fmaxf(0,s.lod_min),.maxLod=fmaxf(0,s.lod_max),.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
            if(s.border[0]==1 && s.border[1]==1 && s.border[2]==1 && s.border[3]==1) info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            else if(!s.border[0] && !s.border[1] && !s.border[2] && s.border[3]==1) info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            else if(s.ws==NVN_WRAP_MODE_CLAMP_TO_BORDER || s.wt==NVN_WRAP_MODE_CLAMP_TO_BORDER || s.wr==NVN_WRAP_MODE_CLAMP_TO_BORDER)
                for(unsigned j=0;j<4;j++) if(s.border[j]!=0) die("unsupported Vulkan sampler border color");
            pthread_mutex_lock(&texture_lock); VulkanTarget *native=draw_native(&texture.texture)->target; pthread_mutex_unlock(&texture_lock);
            textures[draw.texture_count++]=(VulkanDrawTexture){stage,binding,native,info};
        } else {
            uint8_t *data=calloc(1,65536); if(!data) die("draw uniform snapshot allocation failed");
            if(type==2) {
                if(stage || binding || slot) die("invalid draw support buffer");
                float dimensions[4]={(float)target->d.width,(float)target->d.height,0,0}; memcpy(data+160,dimensions,16);
                float one=1; for(unsigned j=0;j<73;j++) memcpy(data+192+j*16,&one,4);
            } else if(slot==1) {
                uint32_t ctl=program_word(sources[stage],0x20),c1=program_word(sources[stage],0x700),size=program_word(sources[stage],0x6fc);
                if(ctl>header[2+stage] || c1>header[2+stage]-ctl || size>header[2+stage]-ctl-c1 || size>65536) die("invalid draw shader constants");
                memcpy(data,sources[stage]+ctl+c1,size);
            } else if(slot>=3 && slot<17) {
                NVNbufferRange range=state->uniform[stage][slot-3]; if(!range.address || !range.size || range.size>65536) die("draw uniform buffer is unbound");
                check((void *)(uintptr_t)range.address,range.size,1); memcpy(data,(void *)(uintptr_t)range.address,range.size);
            } else die("draw driver constant bank is not implemented");
            uniforms[draw.uniform_count++]=(VulkanDrawUniform){stage,binding,data,65536};
        }
    }
    draw.vertex_shader=records+header[6]*5; draw.vertex_shader_bytes=header[4]; draw.fragment_shader=(const uint32_t *)((const uint8_t *)draw.vertex_shader+header[4]); draw.fragment_shader_bytes=header[5];
    unsigned bpp=target->d.format==NVN_FORMAT_R8?1:target->d.format==NVN_FORMAT_RGBA16F?8:4;
    size_t bytes=(size_t)target->d.width*target->d.height*bpp; if(bytes>(64u<<20)) die("draw readback size bound");
    uint8_t *before=malloc(bytes),*after=malloc(bytes); if(!before || !after) die("draw readback allocation failed"); char error[256];
    pthread_mutex_lock(&texture_lock); draw.color=draw_native(target)->target;
    bool ok=vulkan_runtime_pixels(draw.color,false,before,bytes,error) && vulkan_runtime_draw(&draw,after,bytes,error);
    pthread_mutex_unlock(&texture_lock); size_t changed=0,varying=0;
    if(ok) for(size_t i=0;i<bytes;i+=bpp) {changed+=memcmp(before+i,after+i,bpp)!=0; varying+=memcmp(after,after+i,bpp)!=0;}
    const char *capture=getenv("NSW_VULKAN_CAPTURE");
    if(ok && capture && !strcmp(capture,"1")) {
        static size_t captured_bytes; static bool captured_varying,captured_visible;
        uint64_t sequence=vulkan_runtime_draw_count();
        if(sequence<=32 && bytes<=(128u<<20)-captured_bytes) {
            char name[64]; snprintf(name,sizeof name,"draw-%llu.raw",(unsigned long long)sequence);
            shader_capture_file(name,after,bytes); captured_bytes+=bytes;
        }
        size_t visible_varying=0;
        if(varying && !captured_visible) {
            VulkanColorFormat format=target->d.format==NVN_FORMAT_R8?VULKAN_COLOR_R8:target->d.format==NVN_FORMAT_R11G11B10F?VULKAN_COLOR_RGB11F:
                target->d.format==NVN_FORMAT_RGBA16F?VULKAN_COLOR_RGBA16F:target->d.format==NVN_FORMAT_RGBA8_SRGB?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8;
            float first[4]; vulkan_color_decode(format,after,first); long reference[3];
            for(unsigned j=0;j<3;j++) reference[j]=lroundf(fmaxf(0,fminf(1,first[j]))*255);
            for(size_t i=0;i<bytes;i+=bpp) {
                if(!memcmp(after,after+i,bpp)) continue;
                float pixel[4]; vulkan_color_decode(format,after+i,pixel);
                for(unsigned j=0;j<3;j++) if(lroundf(fmaxf(0,fminf(1,pixel[j]))*255)!=reference[j]) {visible_varying++; break;}
            }
        }
        if((varying && !captured_varying) || (visible_varying && !captured_visible)) {
            char metadata[512];
            int length=snprintf(metadata,sizeof metadata,
                "{\"draw\":%llu,\"width\":%u,\"height\":%u,\"nvn_format\":%u,\"bytes_per_pixel\":%u,\"changed_pixels\":%llu,\"varying_pixels\":%llu,\"vertex_source_index\":\"%016llx\",\"fragment_source_index\":\"%016llx\",\"visible_varying_pixels\":%llu,\"displayed\":false}\n",
                (unsigned long long)sequence,target->d.width,target->d.height,target->d.format,bpp,(unsigned long long)changed,(unsigned long long)varying,
                (unsigned long long)shader_index(sources[0],header[2]),(unsigned long long)shader_index(sources[1],header[3]),(unsigned long long)visible_varying);
            if(length<0 || (size_t)length>=sizeof metadata) die("draw metadata exceeds bound");
            if(varying && !captured_varying) {shader_capture_file("first-varying.raw",after,bytes); shader_capture_file("first-varying.json",metadata,(size_t)length); captured_varying=true;}
            if(visible_varying && !captured_visible) {shader_capture_file("first-visible.raw",after,bytes); shader_capture_file("first-visible.json",metadata,(size_t)length); captured_visible=true;}
        }
    }
    for(unsigned i=0;i<draw.uniform_count;i++) free((void *)uniforms[i].data); free(before); free(after); free(packet);
    if(!ok) die("NVN Vulkan draw: %s",error);
    fprintf(stderr,"NVN Vulkan shader draw completed: %ux%u format=%u indices=%u instances=%u textures=%u changed_pixels=%llu varying_pixels=%llu\n",target->d.width,target->d.height,target->d.format,draw.count,draw.instances,draw.texture_count,(unsigned long long)changed,(unsigned long long)varying);
}
