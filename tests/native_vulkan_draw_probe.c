#include "vulkan_draw.h"
#include "vulkan_windows.h"
#include "vulkan_draw_spirv.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    char error[256]; VulkanLoader loader; VulkanDevice device;
    if(!vulkan_windows_load(&loader,error)) {fprintf(stderr,"%s\n",error); return 78;}
    VulkanHost *host=vulkan_host_create(loader.get_instance_proc,-1,error);
    if(!host || !vulkan_host_device(host,&device)) {fprintf(stderr,"%s\n",error); return 78;}
    VulkanRender *render=vulkan_render_create(&device,error); if(!render) {fprintf(stderr,"%s\n",error); return 78;}
    VulkanTarget *target=vulkan_render_target(render,64,64,error); if(!target) return 78;
    const float black[4]={0,0,0,1};
    if(!vulkan_render_clear(render,target,black,NULL,0,error)) return 78;
    const float vertices[6]={-0.75f,-0.75f,0.75f,-0.75f,0,0.75f}; const uint32_t indices[3]={0,1,2};
    VulkanDraw draw={.color=target,.vertex_shader=synthetic_vertex_spirv,.vertex_shader_bytes=sizeof synthetic_vertex_spirv,
        .fragment_shader=synthetic_fragment_spirv,.fragment_shader_bytes=sizeof synthetic_fragment_spirv,
        .vertices={{vertices,sizeof vertices,8}},.attributes={{0,0,VK_FORMAT_R32G32_SFLOAT,0}},.attribute_count=1,
        .indices=indices,.index_bytes=sizeof indices,.index_type=VK_INDEX_TYPE_UINT32,.count=3,.instances=1,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewport={0,0,64,64,0,1},.scissor={{0,0},{64,64}},.cull=VK_CULL_MODE_BACK_BIT,.front=VK_FRONT_FACE_CLOCKWISE,
        .blend={.colorWriteMask=15},.sample_mask=UINT32_MAX};
    unsigned char pixels[64*64*4];
    if(!vulkan_render_draw(render,&draw,pixels,sizeof pixels,error)) {fprintf(stderr,"%s\n",error); return 78;}
    unsigned changed=0; const float expected[4]={0.25f,0.5f,0.75f,1};
    for(unsigned i=0;i<64*64;i++) {
        if(pixels[i*4]) {if(!vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+i*4,expected)) return 78; changed++;}
        else if(!vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+i*4,black)) return 78;
    }
    if(changed!=1152 || !vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+(32*64+32)*4,expected)) {fprintf(stderr,"coverage mismatch: %u\n",changed); return 78;}
    unsigned char coverage[64*64]; for(unsigned i=0;i<64*64;i++) coverage[i]=pixels[i*4]!=0;
    uint32_t blended_shader[sizeof synthetic_fragment_spirv/4]; memcpy(blended_shader,synthetic_fragment_spirv,sizeof blended_shader);
    bool alpha_found=false;
    for(unsigned i=5;i<sizeof blended_shader/4;i+=blended_shader[i]>>16) {
        if((blended_shader[i]&65535)==43 && blended_shader[i+2]==11) {blended_shader[i+3]=0x3f000000; alpha_found=true;}
    }
    if(!alpha_found) return 78;
    const float background[4]={0.1f,0.2f,0.3f,0.4f},blended[4]={0.175f,0.35f,0.525f,0.45f};
    if(!vulkan_render_clear(render,target,background,NULL,0,error)) return 78;
    draw.fragment_shader=blended_shader;
    draw.blend=(VkPipelineColorBlendAttachmentState){.colorWriteMask=15,.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    if(!vulkan_render_draw(render,&draw,pixels,sizeof pixels,error)) {fprintf(stderr,"alpha blend: %s\n",error); return 78;}
    for(unsigned i=0;i<64*64;i++) if(!vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+i*4,coverage[i]?blended:background)) return 78;
    draw.blend=(VkPipelineColorBlendAttachmentState){.colorWriteMask=15};
    VulkanTarget *depth=vulkan_render_depth_target(render,64,64,error);
    if(!depth || !vulkan_render_clear_depth(render,depth,0.25f,true,7,255,NULL,0,error)) return 78;
    VulkanDrawTexture texture={.set=1,.binding=0,.target=depth,.sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE}};
    draw.depth=depth; draw.textures=&texture; draw.texture_count=1;
    draw.fragment_shader=synthetic_depth_fragment_spirv; draw.fragment_shader_bytes=sizeof synthetic_depth_fragment_spirv;
    draw.depth_state=(VkPipelineDepthStencilStateCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_ALWAYS,.stencilTestEnable=VK_TRUE,
        .front={.compareOp=VK_COMPARE_OP_EQUAL,.compareMask=255,.reference=7},.back={.compareOp=VK_COMPARE_OP_EQUAL,.compareMask=255,.reference=7}};
    for(unsigned pass=0;pass<2;pass++) {
        if(!vulkan_render_clear(render,target,black,NULL,0,error)) return 78;
        draw.depth_state.front.reference=pass?0:7;
        if(!vulkan_render_draw(render,&draw,pixels,sizeof pixels,error)) {fprintf(stderr,"depth sampled draw: %s\n",error); return 78;}
        unsigned colored=0;
        for(unsigned i=0;i<64*64;i++) {
            if(pixels[i*4]) {if(!vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+i*4,expected)) return 78; colored++;}
            else if(!vulkan_color_matches(VULKAN_COLOR_RGBA8,pixels+i*4,black)) return 78;
        }
        if(colored!=(pass?0:1152)) {fprintf(stderr,"stencil coverage mismatch %u\n",colored); return 78;}
    }
    if(!vulkan_render_close(render) || !vulkan_host_close(host)) return 78; vulkan_windows_unload(&loader);
    printf("{\"synthetic_draw_verified\":true,\"indexed_triangles\":4,\"changed_pixels\":%u,\"verified_pixels\":16384,\"alpha_blend_verified\":true,\"depth_sampling_verified\":true,\"stencil_rejection_verified\":true,\"game_executed\":false}\n",changed);
    return 0;
}
