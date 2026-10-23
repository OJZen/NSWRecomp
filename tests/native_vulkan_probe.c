// Optional hardware check. This binary never loads a game or creates a window.
#include "vulkan_windows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef NSW_RENDER_PROBE
#include "vulkan_render.h"
#endif

static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if (*p < 32) printf("\\u%04x", *p); else putchar(*p);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    int adapter = -1;
    if (argc == 2) {
        char *end; long value = strtol(argv[1], &end, 10);
        if (*end || value < -1 || value >= 32) return 2;
        adapter = (int)value;
    } else if (argc != 1) return 2;
    char error[256]; VulkanLoader loader;
    if (!vulkan_windows_load(&loader, error)) { fprintf(stderr, "%s\n", error); return 78; }
    VulkanHost *host = vulkan_host_create(loader.get_instance_proc, adapter, error);
    if (!host) { fprintf(stderr, "%s\n", error); vulkan_windows_unload(&loader); return 78; }
    VulkanHostInfo info = *vulkan_host_info(host);
    const uint32_t sizes[] = {4, 4096, 65536, 1048576};
    VulkanTransferProof proofs[4];
    for (unsigned i = 0; i < 4; i++) {
        if (!vulkan_host_transfer(host, sizes[i], 0x1234abcdu ^ (i * 0x31415927u), 5000000000ull, &proofs[i], error)) {
            fprintf(stderr, "%s\n", error);
            if (vulkan_host_close(host)) vulkan_windows_unload(&loader);
            return 78;
        }
    }
#ifdef NSW_RENDER_PROBE
    VulkanDevice device; VulkanRender *render = NULL;
    if (!vulkan_host_device(host, &device) || !(render = vulkan_render_create(&device, error))) {
        fprintf(stderr, "%s\n", error); return 78;
    }
    const uint32_t extents[][2] = {{16, 8}, {127, 65}, {1280, 720}, {1920, 1080}, {1280,720}, {127,65}};
    uint64_t verified_pixels = 0;
    for (unsigned i = 0; i < 6; i++) {
        VulkanColorFormat format=i==5?VULKAN_COLOR_R8:i==4?VULKAN_COLOR_RGB11F:i==3?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8;
        VulkanTarget *target = vulkan_render_target_format(render, extents[i][0], extents[i][1], format, error);
        unsigned bpp=vulkan_color_bytes(format); size_t bytes = (size_t)extents[i][0] * extents[i][1] * bpp;
        unsigned char *pixels = malloc(bytes);
        if (!target || !pixels) return 78;
        for (unsigned pass = 0; pass < 2; pass++) {
            float color[4] = {pass ? 0 : 1, pass ? 1 : 0, 0, 1};
            if(i==3) {color[0]=0.18f; color[1]=0.5f; color[2]=pass?0.003f:0.8f; color[3]=0.5f;}
            if(i==4) {color[0]=pass?4.f:1.f; color[1]=0.5f; color[2]=0.25f;}
            if (!vulkan_render_clear(render, target, color, pixels, bytes, error)) {
                fprintf(stderr, "%s\n", error); return 78;
            }
            for (size_t p = 0; p < bytes; p+=bpp) if (!vulkan_color_matches(format,pixels+p,color)) {
                fputs("Color-attachment readback mismatch\n", stderr); return 78;
            }
            if(i==4) {uint32_t packed; memcpy(&packed,pixels,4); if(packed!=(pass?0x681c0440u:0x681c03c0u)) return 78;}
            verified_pixels += bytes / bpp;
        }
        VulkanTarget *copy=vulkan_render_target_format(render,extents[i][0],extents[i][1],format,error);
        const float before[4]={0,0,1,0};
        if(!copy || !vulkan_render_clear(render,copy,before,NULL,0,error) || !vulkan_render_copy(render,target,copy,false,pixels,bytes,error)) {
            fprintf(stderr,"color copy: %s\n",error); return 78;
        }
        if(!vulkan_render_release(render,copy)) return 78;
        if(i==4) {
            VulkanTarget *converted=vulkan_render_target_format(render,extents[i][0],extents[i][1],VULKAN_COLOR_RGBA16F,error);
            uint16_t *halves=malloc((size_t)extents[i][0]*extents[i][1]*8);
            if(!converted || !halves || !vulkan_render_copy(render,target,converted,false,halves,(size_t)extents[i][0]*extents[i][1]*8,error)) {fprintf(stderr,"float conversion: %s\n",error); return 78;}
            const uint16_t expected[4]={0x4400,0x3800,0x3400,0x3c00};
            for(size_t p=0;p<(size_t)extents[i][0]*extents[i][1]*4;p++) if(halves[p]!=expected[p%4]) return 78;
            free(halves); if(!vulkan_render_release(render,converted)) return 78;
            // HDR values clamp for UNORM; sRGB uses nonlinear encoding for the
            // other channels. Expectations are independent of the copy helper.
            const unsigned char encoded[2][4]={{255,128,64,255},{255,188,137,255}};
            for(unsigned srgb=0;srgb<2;srgb++) {
                converted=vulkan_render_target_format(render,extents[i][0],extents[i][1],srgb?VULKAN_COLOR_SRGBA8:VULKAN_COLOR_RGBA8,error);
                if(!converted || !vulkan_render_copy(render,target,converted,false,pixels,bytes,error)) {fprintf(stderr,"UNORM conversion: %s\n",error); return 78;}
                for(size_t p=0;p<bytes;p++) if(abs((int)pixels[p]-encoded[srgb][p%4])>1) return 78;
                if(!vulkan_render_release(render,converted)) return 78;
            }
        }
        free(pixels);
        if (!vulkan_render_release(render, target)) return 78;
    }
    const size_t depth_pixels=1280*720; unsigned char *depth_data=malloc(depth_pixels*5);
    VulkanTarget *depth=vulkan_render_depth_target(render,1280,720,error);
    if(!depth || !depth_data) {fprintf(stderr,"%s\n",error); return 78;}
    for(unsigned pass=0;pass<3;pass++) {
        if(!vulkan_render_clear_depth(render,depth,pass?0.75f:0.25f,pass<2,pass==2?17:91,pass==1?0:255,depth_data,depth_pixels*5,error)) {
            fprintf(stderr,"D24S8: %s\n",error); return 78;
        }
        uint32_t expected=pass?0xbfffff:0x400000;
        for(size_t i=0;i<depth_pixels;i++) {uint32_t value; memcpy(&value,depth_data+i*4,4);
            if(llabs((int64_t)(value&0xffffff)-expected)>1 || depth_data[depth_pixels*4+i]!=(pass==2?17:91)) {
                fputs("Depth/stencil preserved-aspect mismatch\n",stderr); return 78;
            }
        }
    }
    VulkanTarget *depth_copy=vulkan_render_depth_target(render,1280,720,error);
    if(!depth_copy || !vulkan_render_clear_depth(render,depth_copy,0.25f,true,91,255,NULL,0,error) ||
       !vulkan_render_copy(render,depth,depth_copy,false,depth_data,depth_pixels*5,error)) {fprintf(stderr,"depth copy: %s\n",error); return 78;}
    for(size_t i=0;i<depth_pixels;i++) {uint32_t value; memcpy(&value,depth_data+i*4,4); if(llabs((int64_t)(value&0xffffff)-0xbfffff)>1 || depth_data[depth_pixels*4+i]!=17) return 78;}
    free(depth_data); if(!vulkan_render_release(render,depth) || !vulkan_render_release(render,depth_copy)) return 78;
    // Nonuniform inputs distinguish nearest depth/stencil resize from a clear,
    // and bilinear color resize from either a clear or a nearest copy.
    VulkanTarget *ds_source=vulkan_render_depth_target(render,8,6,error),*ds_dest=vulkan_render_depth_target(render,4,3,error);
    unsigned char ds_input[48*5],ds_output[12*5];
    for(unsigned i=0;i<48;i++) {uint32_t value=i*10000; memcpy(ds_input+i*4,&value,4); ds_input[48*4+i]=(uint8_t)i;}
    if(!ds_source || !ds_dest || !vulkan_render_pixels(render,ds_source,true,ds_input,sizeof ds_input,error) || !vulkan_render_copy(render,ds_source,ds_dest,false,ds_output,sizeof ds_output,error)) {fprintf(stderr,"depth resize: %s\n",error); return 78;}
    for(unsigned y=0;y<3;y++) for(unsigned x=0;x<4;x++) {unsigned to=y*4+x,from=(y*2+1)*8+x*2+1; uint32_t value; memcpy(&value,ds_output+to*4,4); if((value&0xffffff)!=from*10000 || ds_output[12*4+to]!=from) return 78;}
    if(!vulkan_render_release(render,ds_source) || !vulkan_render_release(render,ds_dest)) return 78;
    VulkanTarget *cs_source=vulkan_render_target(render,8,6,error),*cs_dest=vulkan_render_target(render,4,3,error);
    unsigned char cs_input[48*4],cs_output[12*4];
    for(unsigned y=0;y<6;y++) for(unsigned x=0;x<8;x++) {unsigned i=(y*8+x)*4; cs_input[i]=x*20; cs_input[i+1]=y*40; cs_input[i+2]=(x+y)*10; cs_input[i+3]=255;}
    if(!cs_source || !cs_dest || !vulkan_render_pixels(render,cs_source,true,cs_input,sizeof cs_input,error) || !vulkan_render_copy(render,cs_source,cs_dest,true,cs_output,sizeof cs_output,error)) {fprintf(stderr,"color resize: %s\n",error); return 78;}
    for(unsigned y=0;y<3;y++) for(unsigned x=0;x<4;x++) {unsigned i=(y*4+x)*4; if(cs_output[i]!=x*40+10 || cs_output[i+1]!=y*80+20 || cs_output[i+2]!=(x+y)*20+10 || cs_output[i+3]!=255) return 78;}
    if(!vulkan_render_release(render,cs_source) || !vulkan_render_release(render,cs_dest)) return 78;
    if (!vulkan_render_close(render)) return 78;
#endif
    if (!vulkan_host_close(host)) { fputs("Vulkan submission still pending\n", stderr); return 78; }
    vulkan_windows_unload(&loader);
    printf("{\"status\":\"vulkan_transfer_verified\",\"device_name\":"); json_string(info.device.deviceName);
    printf(",\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,\"driver_version\":%u,"
           "\"adapter_index\":%u,\"queue_family\":%u,\"game_executed\":false,\"rendering_verified\":false,\"transfers\":[",
           info.device.vendorID, info.device.deviceID, info.device.apiVersion, info.device.driverVersion,
           info.adapter_index, info.queue_family);
    for (unsigned i = 0; i < 4; i++) printf("%s{\"bytes\":%u,\"pattern\":%u,\"source_memory_type\":%u,"
        "\"readback_memory_type\":%u,\"coherent\":%s}", i ? "," : "", proofs[i].bytes, proofs[i].pattern,
        proofs[i].source_memory_type, proofs[i].readback_memory_type, proofs[i].coherent ? "true" : "false");
    printf("]");
#ifdef NSW_RENDER_PROBE
    printf(",\"color_attachment_clear_verified\":true,\"srgb_clear_verified\":true,\"rgb11f_clear_verified\":true,\"r8_clear_verified\":true,\"clear_passes\":12,\"verified_pixels\":%llu,\"depth_stencil_verified\":true,\"depth_clear_passes\":3,\"depth_verified_pixels\":2764800", (unsigned long long)verified_pixels);
    printf(",\"image_copy_verified\":true,\"image_copies\":10,\"rgb11_to_rgba16f_verified\":true,\"hdr_to_unorm_srgb_verified\":true");
    printf(",\"nonuniform_depth_resize_verified\":true,\"nonuniform_linear_resize_verified\":true");
#endif
    puts("}");
    return 0;
}
