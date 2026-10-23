// Descriptor registration takes immutable metadata snapshots. Shader descriptor
// consumption is deferred until the Vulkan shader/pipeline backend is provided.
typedef struct {uint64_t address; tex_t texture; view_t view; samp_par sampler; unsigned kind;} Descriptor;
static Descriptor descriptors[4096];
static pthread_mutex_t descriptor_lock=PTHREAD_MUTEX_INITIALIZER;
static void descriptor_forget(const dpool_t *p) {
    uint64_t start=(uintptr_t)pool_range((const NVNmemoryPool *)p->pool,p->off,(size_t)p->n*32,3);
    pthread_mutex_lock(&descriptor_lock);
    for(unsigned i=0;i<4096;i++) if(descriptors[i].address>=start && descriptors[i].address-start<(uint64_t)p->n*32) descriptors[i]=(Descriptor){0};
    pthread_mutex_unlock(&descriptor_lock);
}
static void descriptor_store(const void *pool,int id,Descriptor snapshot) {
    const dpool_t *p=descriptor_check(pool); if(id<256 || id>=p->n) die("NVN descriptor index outside pool");
    snapshot.address=(uintptr_t)pool_range((const NVNmemoryPool *)p->pool,p->off+(ptrdiff_t)id*32,32,3);
    pthread_mutex_lock(&descriptor_lock); Descriptor *slot=NULL;
    for(unsigned i=0;i<4096;i++) if(descriptors[i].address==snapshot.address) {slot=&descriptors[i]; break;}
    if(!slot) for(unsigned i=0;i<4096;i++) if(!descriptors[i].address) {slot=&descriptors[i]; break;}
    if(!slot) {pthread_mutex_unlock(&descriptor_lock); die("NVN registered descriptor bound reached");}
    *slot=snapshot; uint64_t token[4]={MAGIC('V','K','D','S'),(uint64_t)(slot-descriptors)+1,snapshot.kind,snapshot.texture.serial};
    memcpy((void *)(uintptr_t)snapshot.address,token,sizeof token); pthread_mutex_unlock(&descriptor_lock);
}
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *pool,int id,const NVNtexture *texture,const NVNtextureView *view) {
    Descriptor value={.kind=1,.texture=*texture_check(texture)}; if(view) value.view=*READ(view_t,view); descriptor_store(pool,id,value);
}
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *pool,int id,const NVNtexture *texture,const NVNtextureView *view) {
    Descriptor value={.kind=2,.texture=*texture_check(texture)}; if(view) value.view=*READ(view_t,view); descriptor_store(pool,id,value);
}
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *device,int texture,int sampler) {
    device_check(device); if(texture<256 || texture>=1048576 || sampler<256 || sampler>=4096) die("invalid NVN texture handle indices");
    return (uint64_t)texture|((uint64_t)sampler<<20);
}
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *device,int texture) {device_check(device); if(texture<256 || texture>=1048576) die("invalid NVN texel handle index"); return texture;}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *device,int texture) {return h_nvnDeviceGetTexelFetchHandle(device,texture);}
#define SB(b) (&WRITE(samp_builder_t,b)->p)
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b,NVNdevice *device) {device_check(device); WRITE(samp_builder_t,b)->dev=device;}
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    samp_builder_t *o=WRITE(samp_builder_t,b); NVNdevice *d=o->dev; *o=(samp_builder_t){.dev=d};
    o->p=(samp_par){.minf=NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR,.magf=NVN_MAG_FILTER_LINEAR,
        .ws=NVN_WRAP_MODE_REPEAT,.wt=NVN_WRAP_MODE_REPEAT,.wr=NVN_WRAP_MODE_REPEAT,.cmp_func=NVN_COMPARE_FUNC_LESS,.lod_min=-1000,.lod_max=1000,.aniso=1};
}
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b,NVNminFilter min,NVNmagFilter mag) {SB(b)->minf=min; SB(b)->magf=mag;}
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b,NVNwrapMode s,NVNwrapMode t,NVNwrapMode r) {samp_par *p=SB(b); p->ws=s; p->wt=t; p->wr=r;}
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b,float min,float max) {SB(b)->lod_min=min; SB(b)->lod_max=max;}
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b,float value) {SB(b)->lod_bias=value;}
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b,float value) {SB(b)->aniso=value;}
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b,NVNcompareMode mode,NVNcompareFunc func) {SB(b)->cmp_mode=mode; SB(b)->cmp_func=func;}
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b,NVNsamplerReduction reduction) {SB(b); if(reduction!=0) die("NVN min/max sampler reduction is unsupported");}
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b,float snap) {SB(b); if(snap!=0) die("NVN sampler LOD snapping is unsupported");}
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b,const float *color) {check(color,16,1); memcpy(SB(b)->border,color,16);}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s,const NVNsamplerBuilder *b) {
    const samp_builder_t *o=READ(samp_builder_t,b); device_check(o->dev); *WRITE(samp_t,s)=(samp_t){.magic=MAGIC('V','K','S','A'),.p=o->p}; return 1;
}
static const samp_t *sampler_check(const NVNsampler *s) {const samp_t *o=READ(samp_t,s); if(o->magic!=MAGIC('V','K','S','A')) die("invalid NVN sampler"); return o;}
NVN(nvnSamplerFinalize, void, NVNsampler *s) {sampler_check(s); WRITE(samp_t,s)->magic=0;}
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s,const char *label) {sampler_check(s); debug_label(label);}
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *pool,int id,const NVNsampler *sampler) {descriptor_store(pool,id,(Descriptor){.kind=3,.sampler=sampler_check(sampler)->p});}
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *pool,int id,const NVNsamplerBuilder *b) {descriptor_store(pool,id,(Descriptor){.kind=3,.sampler=READ(samp_builder_t,b)->p});}
#undef SB
