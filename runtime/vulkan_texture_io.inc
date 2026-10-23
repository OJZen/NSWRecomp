// CPU texture writes use the shared Tegra block-linear address calculation.
#include "graphics/tex/nvn_deswizzle.h"
static void texture_invalidate_native(uint64_t serial) {
    pthread_mutex_lock(&texture_lock);
    for(unsigned i=0;i<64;i++) if(native_textures[i].serial==serial) {
        if(!vulkan_runtime_release(native_textures[i].target)) {pthread_mutex_unlock(&texture_lock); die("NVN texture update while GPU work is pending");}
        native_textures[i]=(NativeTexture){0}; break;
    }
    pthread_mutex_unlock(&texture_lock);
}
static void texture_write_cpu(const NVNtexture *texture,const NVNtextureView *view,const NVNcopyRegion *region,
    const void *source,ptrdiff_t row_stride,ptrdiff_t image_stride) {
    const tex_t *t=texture_check(texture); const view_t *v=view?READ(view_t,view):NULL; check(region,sizeof *region,1);
    if(region->width<0 || region->height<0 || region->depth<0) die("invalid NVN texture write extent");
    if(!region->width || !region->height || !region->depth) return;
    unsigned level=v && v->set_levels?(unsigned)v->base_level:0, layer=v && v->set_layers?(unsigned)v->min_layer:0;
    uint32_t row_bytes,rows,slices,bpb; uint64_t base; unsigned th,td;
    bool linear=t->d.flags&0x50;
    if(level>=t->d.levels || level>=15 || layer>=t->L.layers || (v && v->set_format && v->format!=t->d.format)) die("invalid NVN texture write view");
    int bw,bh; bpb=(uint32_t)nvn_fmt_bpb(t->d.format,&bw,&bh);
    if(!bpb) die("unknown NVN texture write format");
    unsigned width=t->d.width>>level, height=t->d.height>>level; if(!width) width=1; if(!height) height=1;
    if(linear) {
        if(t->d.target!=NVN_TEXTURE_TARGET_2D || level || layer || t->d.depth!=1 || t->d.samples>1) die("unsupported linear NVN write");
        row_bytes=(uint32_t)(t->L.size/t->d.height); rows=t->d.height; slices=1; base=0; th=td=0;
    } else if(nvn_deswizzle_geom(&t->d,level,layer,&row_bytes,&rows,&slices,&base,&th,&td,&bpb)) die("unsupported NVN texture write geometry");
    unsigned available=t->d.target==NVN_TEXTURE_TARGET_3D?slices:t->L.layers-layer;
    if(v && v->set_layers && v->nlayers>0 && (unsigned)v->nlayers<available) available=v->nlayers;
    if(region->xoffset<0 || region->yoffset<0 || region->zoffset<0 || (uint64_t)region->xoffset+region->width>width ||
        (uint64_t)region->yoffset+region->height>height || (uint64_t)region->zoffset+region->depth>available || region->xoffset%bw || region->yoffset%bh) die("NVN texture write outside subresource");
    size_t write_bytes=((unsigned)region->width+bw-1)/bw*bpb, write_rows=((unsigned)region->height+bh-1)/bh;
    if(!row_stride) row_stride=write_bytes;
    if(row_stride<(ptrdiff_t)write_bytes || (uint64_t)row_stride>(128u<<20)/write_rows) die("NVN texture write row stride exceeds bound");
    if(!image_stride) image_stride=write_rows*row_stride;
    if(image_stride<(ptrdiff_t)(write_rows*row_stride) || (uint64_t)image_stride>(128u<<20)/(unsigned)region->depth) die("NVN texture write image stride exceeds bound");
    size_t source_bytes=(region->depth-1)*(size_t)image_stride+(write_rows-1)*(size_t)row_stride+write_bytes;
    check(source,source_bytes,1); pool_range((const NVNmemoryPool *)t->pool,t->off,t->L.size,3);
    if((uintptr_t)source<(uintptr_t)t->mem+t->L.size && (uintptr_t)t->mem<(uintptr_t)source+source_bytes) die("overlapping NVN texture write source");
    texture_invalidate_native(t->serial);
    for(unsigned z=0;z<(unsigned)region->depth;z++) {
        unsigned dst_layer=t->d.target==NVN_TEXTURE_TARGET_3D?layer:layer+region->zoffset+z;
        unsigned dst_z=t->d.target==NVN_TEXTURE_TARGET_3D?region->zoffset+z:0;
        if(!linear && nvn_deswizzle_geom(&t->d,level,dst_layer,&row_bytes,&rows,&slices,&base,&th,&td,&bpb)) die("NVN texture write geometry changed");
        for(size_t y=0;y<write_rows;y++) for(size_t x=0;x<write_bytes;) {
            unsigned dx=(unsigned)region->xoffset/bw*bpb+(unsigned)x, dy=(unsigned)region->yoffset/bh+(unsigned)y;
            size_t n=16-(dx&15); if(n>write_bytes-x) n=write_bytes-x;
            uint64_t offset=base+(linear?(uint64_t)dy*row_bytes+dx:nvn_bl_offset(dx,dy,dst_z,row_bytes,rows,th,td));
            if(offset>t->L.size || n>t->L.size-offset) die("NVN texture storage write overflow");
            memcpy(t->mem+offset,(const uint8_t *)source+z*(size_t)image_stride+y*(size_t)row_stride+x,n); x+=n;
        }
    }
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *texture,const NVNtextureView *view,const NVNcopyRegion *region,const void *data) {texture_write_cpu(texture,view,region,data,0,0);}
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *texture,const NVNtextureView *view,const NVNcopyRegion *region,const void *data,ptrdiff_t row,ptrdiff_t image) {texture_write_cpu(texture,view,region,data,row,image);}
NVN(nvnTextureFlushTexels, void, const NVNtexture *texture,const NVNtextureView *view,const NVNcopyRegion *region) {
    const tex_t *t=texture_check(texture); if(view) check(view,sizeof(view_t),1); if(region) check(region,sizeof *region,1); texture_invalidate_native(t->serial);
}
