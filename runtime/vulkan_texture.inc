// NVN texture metadata uses the shared Tegra layout calculator. Vulkan images
// are allocated lazily on first GPU use, independently of the guest pool bytes.
#include <stdatomic.h>
#define TB(b) WRITE(tex_builder_t,b)
#define CTB(b) READ(tex_builder_t,b)
static atomic_uint_fast64_t texture_serial;
typedef struct { uint64_t serial; VulkanTarget *target; } NativeTexture;
static NativeTexture native_textures[64];
static pthread_mutex_t texture_lock=PTHREAD_MUTEX_INITIALIZER;
static const tex_t *texture_check(const NVNtexture *t) {
    const tex_t *o=READ(tex_t,t); if(o->magic!=MAGIC('V','K','T','X') || !o->serial) die("invalid NVN texture"); return o;
}
static nvn_texlayout texture_layout(const tex_builder_t *b) {
    const nvn_texdesc *d=&b->d;
    if (!d->width || !d->height || !d->depth || d->width>16384 || d->height>16384 || d->depth>2048 || !d->levels || d->levels>15 ||
        (unsigned)d->target>10 || d->pkg_block_h<0 || d->pkg_block_h>6 || d->samples<0 || d->samples>16 || d->stride>(128u<<20))
        die("invalid NVN texture dimensions or layout");
    nvn_texlayout layout; nvn_tex_layout(d,&layout);
    if (!layout.bpb || !layout.size || layout.size>(512u<<20)) die("unsupported NVN texture format or storage size");
    return layout;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { device_check(d); TB(b)->dev=d; }
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    tex_builder_t *o=TB(b); NVNdevice *d=o->dev; *o=(tex_builder_t){.dev=d};
    o->d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.width=1,.height=1,.depth=1,.levels=1};
}
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int flags) { TB(b)->d.flags=flags; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget target) { TB(b)->d.target=target; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int value) { TB(b)->d.width=value; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int value) { TB(b)->d.height=value; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int value) { TB(b)->d.depth=value; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int width) { tex_builder_t *o=TB(b); o->d.width=width; o->d.height=o->d.depth=1; }
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int width, int height) { tex_builder_t *o=TB(b); o->d.width=width; o->d.height=height; o->d.depth=1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int width, int height, int depth) { tex_builder_t *o=TB(b); o->d.width=width; o->d.height=height; o->d.depth=depth; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int value) { TB(b)->d.levels=value; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int value) { TB(b)->d.samples=value; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat value) { TB(b)->d.format=value; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t stride) { if(stride<0 || stride>(128u<<20)) die("invalid texture stride"); TB(b)->d.stride=(uint32_t)stride; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t offset) { tex_builder_t *o=TB(b); o->pool=(pool_t *)p; o->off=offset; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *layout) {
    tex_builder_t *o=TB(b); o->has_layout=layout!=NULL;
    if(layout) {check(layout,8,1); memcpy(o->layout,layout->layout,8);} o->d.pkg_block_h=layout?layout->layout[0]+1:0;
}
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->packaged_data=data; }
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) { return texture_layout(CTB(b)).size; }
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) { return texture_layout(CTB(b)).align; }
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return CTB(b)->d.flags; }
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s=CTB(b); device_check(s->dev); nvn_texlayout layout=texture_layout(s);
    if (s->off<0 || (uint64_t)s->off%layout.align || (s->d.flags&NVN_TEXTURE_FLAGS_SPARSE)) return 0;
    uint8_t *memory=pool_range((const NVNmemoryPool *)s->pool,s->off,layout.size,3);
    if(s->packaged_data && s->packaged_data!=memory) die("packaged NVN data must match texture storage");
    tex_t *o=WRITE(tex_t,t); *o=(tex_t){.magic=MAGIC('V','K','T','X'),.has_layout=s->has_layout,.swizzle=s->swizzle,
        .d=s->d,.L=layout,.mem=memory,.pool=s->pool,.off=s->off,.serial=atomic_fetch_add(&texture_serial,1)+1};
    memcpy(o->layout,s->layout,8);
    fprintf(stderr,"NVN texture initialized %ux%u format=%u serial=%llu\n",o->d.width,o->d.height,o->d.format,(unsigned long long)o->serial);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) {
    uint64_t serial=texture_check(t)->serial; pthread_mutex_lock(&texture_lock);
    for(unsigned i=0;i<64;i++) if(native_textures[i].serial==serial) {
        if(!vulkan_runtime_release(native_textures[i].target)) {pthread_mutex_unlock(&texture_lock); die("NVN texture still in flight");}
        native_textures[i]=(NativeTexture){0}; break;
    }
    pthread_mutex_unlock(&texture_lock); WRITE(tex_t,t)->magic=0;
}
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *label) { texture_check(t); debug_label(label); }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return texture_check(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return texture_check(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return texture_check(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return texture_check(t)->d.depth; }
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return texture_check(t)->d.levels; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return texture_check(t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return texture_check(t)->d.target; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)texture_check(t)->L.size; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return texture_check(t)->off; }
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *view) { *WRITE(view_t,view)=(view_t){0}; }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *view, int first, int count) {
    if(first<0 || first>14 || count < -1 || count>15) die("invalid texture view levels");
    view_t *o=WRITE(view_t,view); o->set_levels=1; o->base_level=first; o->nlevels=count;
}
NVN(nvnTextureViewSetLayers, void, NVNtextureView *view, int first, int count) {
    if(first<0 || count < -1 || !count) die("invalid texture view layers");
    view_t *o=WRITE(view_t,view); o->set_layers=1; o->min_layer=first; o->nlayers=count;
}
NVN(nvnTextureViewSetFormat, void, NVNtextureView *view, NVNformat value) { if((unsigned)value>255) die("invalid texture view format"); view_t *o=WRITE(view_t,view); o->set_format=1; o->format=value; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *view,int *first,int *count) {
    const view_t *o=READ(view_t,view); if(!o->set_levels) return 0;
    check(first,4,3); check(count,4,3); *first=o->base_level; *count=o->nlevels; return 1;
}
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *view,int *first,int *count) {
    const view_t *o=READ(view_t,view); if(!o->set_layers) return 0;
    check(first,4,3); check(count,4,3); *first=o->min_layer; *count=o->nlayers; return 1;
}
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *view,NVNformat *format) {
    const view_t *o=READ(view_t,view); if(!o->set_format) return 0; check(format,sizeof *format,3); *format=o->format; return 1;
}
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *view,NVNtextureTarget *target) {
    const view_t *o=READ(view_t,view); if(!o->set_target) return 0; check(target,sizeof *target,3); *target=o->target; return 1;
}
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *view,NVNtextureDepthStencilMode *mode) {
    const view_t *o=READ(view_t,view); if(!o->set_depth_mode) return 0; check(mode,sizeof *mode,3); *mode=o->depth_mode; return 1;
}
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *view,NVNtextureSwizzle *r,NVNtextureSwizzle *g,NVNtextureSwizzle *b,NVNtextureSwizzle *a) {
    const view_t *o=READ(view_t,view); if(!o->swizzle) return 0; NVNtextureSwizzle *out[]={r,g,b,a};
    for(unsigned i=0;i<4;i++) {check(out[i],sizeof *out[i],3); *out[i]=(o->swizzle>>(3*i))&7;} return 1;
}
NVN(nvnTextureViewSetTarget, void, NVNtextureView *view, NVNtextureTarget value) { if((unsigned)value>10) die("invalid texture view target"); view_t *o=WRITE(view_t,view); o->set_target=1; o->target=value; }
static uint16_t texture_swizzle(NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle b,NVNtextureSwizzle a) {
    if((unsigned)r>5 || (unsigned)g>5 || (unsigned)b>5 || (unsigned)a>5) die("invalid NVN texture swizzle");
    return (uint16_t)(0x8000|r|(g<<3)|(b<<6)|(a<<9));
}
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *view,NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle b,NVNtextureSwizzle a) {WRITE(view_t,view)->swizzle=texture_swizzle(r,g,b,a);}
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *builder,NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle b,NVNtextureSwizzle a) {TB(builder)->swizzle=texture_swizzle(r,g,b,a);}
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *view,NVNtextureDepthStencilMode mode) {
    if((unsigned)mode>1) die("invalid NVN depth-stencil view mode"); view_t *o=WRITE(view_t,view); o->set_depth_mode=1; o->depth_mode=mode;
}
#undef TB
#undef CTB
