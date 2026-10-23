// Reuse the synthetic driver's device/memory/fence implementation, and add
// render-pass commands. These checks never load a Vulkan driver.
#define main transfer_driver_main
#include "native_vulkan_host.c"
#undef main
#include "vulkan_render.h"
static unsigned images, views, passes, framebuffers, pass_calls;
static bool submitted;
typedef struct {Memory *memory; uint32_t width,height; VkFormat format;} Image;
static Image *active_image;
static float active_color[4];
static VkClearDepthStencilValue depth_value;
static VkImageAspectFlags depth_aspects;
static uint32_t stored_depth;
static uint8_t stored_stencil;
static VkResult VKAPI_CALL rCreateImage(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out) {
    assert((ci->usage&(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT)) && ci->mipLevels==1 && ci->arrayLayers==1);
    Image *image=calloc(1,sizeof *image); assert(image); image->width=ci->extent.width; image->height=ci->extent.height; image->format=ci->format;
    *out=(VkImage)image; images++; return VK_SUCCESS;
}
static void VKAPI_CALL rDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks *a) {assert(!submitted || completed); images--; free((Image *)image);}
static void VKAPI_CALL rGetImageMemoryRequirements(VkDevice d,VkImage image,VkMemoryRequirements *out) {
    Image *i=(Image *)image; *out=(VkMemoryRequirements){.size=(size_t)i->width*i->height*4,.alignment=64,.memoryTypeBits=3};
}
static VkResult VKAPI_CALL rBindImageMemory(VkDevice d,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {assert(!offset); ((Image *)image)->memory=(Memory *)memory; return VK_SUCCESS;}
static VkResult VKAPI_CALL rCreateImageView(VkDevice d,const VkImageViewCreateInfo *ci,const VkAllocationCallbacks *a,VkImageView *out) {
    if(is("view-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(ci->format==((Image *)ci->image)->format); *out=(VkImageView)ci->image; views++; return VK_SUCCESS;
}
static void VKAPI_CALL rDestroyImageView(VkDevice d,VkImageView view,const VkAllocationCallbacks *a) {views--;}
static VkResult VKAPI_CALL rCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *ci,const VkAllocationCallbacks *a,VkRenderPass *out) {
    if(++pass_calls==2 && is("pass-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(ci->attachmentCount==1 && ci->pAttachments->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && ci->pAttachments->storeOp==VK_ATTACHMENT_STORE_OP_STORE);
    assert(ci->pAttachments->initialLayout==VK_IMAGE_LAYOUT_UNDEFINED && ci->pAttachments->finalLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(ci->dependencyCount==2 && ci->pDependencies[1].srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT && ci->pDependencies[1].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
    *out=HANDLE(VkRenderPass,ci->pAttachments->format); passes++; return VK_SUCCESS;
}
static void VKAPI_CALL rDestroyRenderPass(VkDevice d,VkRenderPass pass,const VkAllocationCallbacks *a) {passes--;}
static VkResult VKAPI_CALL rCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo *ci,const VkAllocationCallbacks *a,VkFramebuffer *out) {
    if(is("framebuffer-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    Image *image=(Image *)ci->pAttachments[0]; assert(ci->attachmentCount==1 && (uintptr_t)ci->renderPass==image->format && ci->width==image->width);
    *out=(VkFramebuffer)image; framebuffers++; return VK_SUCCESS;
}
static void VKAPI_CALL rDestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks *a) {framebuffers--;}
static void VKAPI_CALL rCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *bi,VkSubpassContents contents) {
    assert(contents==VK_SUBPASS_CONTENTS_INLINE && bi->clearValueCount==1 && !bi->renderArea.offset.x && !bi->renderArea.offset.y);
    active_image=(Image *)bi->framebuffer; memcpy(active_color,bi->pClearValues->color.float32,16);
    assert(bi->renderArea.extent.width==active_image->width && bi->renderArea.extent.height==active_image->height);
}
static void VKAPI_CALL rCmdEndRenderPass(VkCommandBuffer c) {assert(active_image);}
static void VKAPI_CALL rCmdClearDepthStencilImage(VkCommandBuffer c,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue *value,uint32_t count,const VkImageSubresourceRange *ranges) {
    assert(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count==1 && ranges->layerCount==1 && ranges->levelCount==1);
    active_image=(Image *)image; depth_value=*value; depth_aspects=ranges->aspectMask;
}
static void VKAPI_CALL rCmdCopyImageToBuffer(VkCommandBuffer c,VkImage image,VkImageLayout layout,VkBuffer dest,uint32_t count,const VkBufferImageCopy *regions) {
    assert((Image *)image==active_image && layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count==1 && !regions->bufferRowLength);
    copy_buffer=(Buffer *)dest;
}
static void VKAPI_CALL rCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    uint32_t count,const VkMemoryBarrier *memory,uint32_t bc,const VkBufferMemoryBarrier *b,uint32_t ic,const VkImageMemoryBarrier *i) {
    if(ic) {assert(ic==1 && !count && !bc && i->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && i->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
        assert(i->subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)); barriers++; return;}
    assert(src==VK_PIPELINE_STAGE_TRANSFER_BIT && dst==VK_PIPELINE_STAGE_HOST_BIT && count==1 && !bc);
    assert(memory->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && memory->dstAccessMask==VK_ACCESS_HOST_READ_BIT); barriers++;
}
static VkResult VKAPI_CALL rEndCommandBuffer(VkCommandBuffer c) {assert(barriers==(active_image->format==VK_FORMAT_D24_UNORM_S8_UINT?3u:1u)); return VK_SUCCESS;}
static VkResult VKAPI_CALL rQueueSubmit(VkQueue q,uint32_t count,const VkSubmitInfo *info,VkFence fence) {
    assert(count==1 && info->commandBufferCount==1 && active_image && copy_buffer); submitted=true;
    if(is("submit-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    unsigned char *bytes=copy_buffer->memory->data;
    if(active_image->format==VK_FORMAT_D24_UNORM_S8_UINT) {
        if(depth_aspects&VK_IMAGE_ASPECT_DEPTH_BIT) stored_depth=(uint32_t)llround((double)depth_value.depth*16777215);
        if(depth_aspects&VK_IMAGE_ASPECT_STENCIL_BIT) stored_stencil=(uint8_t)depth_value.stencil;
        size_t n=(size_t)active_image->width*active_image->height;
        for(size_t i=0;i<n;i++) {memcpy(bytes+i*4,&stored_depth,4); bytes[n*4+i]=stored_stencil;}
    } else for(size_t i=0;i<copy_buffer->bytes;i++) bytes[i]=(unsigned char)(active_color[active_image->format==VK_FORMAT_R8_UNORM?0:i%4]*255);
    if(is("render-mismatch")) bytes[31]^=255;
    return VK_SUCCESS;
}
#define RENTRY(name) if(!strcmp(name_text,"vk" #name)) {PFN_vk##name fn=r##name; return (PFN_vkVoidFunction)fn;}
static void VKAPI_CALL rCmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy *p) {abort();}
static void VKAPI_CALL rCmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit *p,VkFilter f) {abort();}
static void VKAPI_CALL rCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer b,VkImage i,VkImageLayout l,uint32_t n,const VkBufferImageCopy *p) {abort();}
static VkResult VKAPI_CALL rFlushMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *r) {abort();}
static PFN_vkVoidFunction VKAPI_CALL render_get(VkDevice d,const char *name_text) {
    if(is("missing-render-entry") && !strcmp(name_text,"vkCmdBeginRenderPass")) return NULL;
    RENTRY(CreateImage) RENTRY(DestroyImage) RENTRY(GetImageMemoryRequirements) RENTRY(BindImageMemory)
    RENTRY(CreateImageView) RENTRY(DestroyImageView) RENTRY(CreateRenderPass) RENTRY(DestroyRenderPass)
    RENTRY(CreateFramebuffer) RENTRY(DestroyFramebuffer) RENTRY(CmdBeginRenderPass) RENTRY(CmdEndRenderPass)
    RENTRY(CmdCopyImageToBuffer) RENTRY(CmdPipelineBarrier) RENTRY(EndCommandBuffer) RENTRY(QueueSubmit) RENTRY(CmdClearDepthStencilImage) RENTRY(CmdCopyImage) RENTRY(CmdBlitImage)
    RENTRY(CmdCopyBufferToImage) RENTRY(FlushMappedMemoryRanges)
    return fGetDeviceProcAddr(d,name_text);
}
int main(int argc,char **argv) {
    assert(argc==2); mode=argv[1]; char error[256];
    VulkanDevice device={.device=HANDLE(VkDevice,2),.queue=HANDLE(VkQueue,3),.get_proc=render_get};
    fGetPhysicalDeviceMemoryProperties(HANDLE(VkPhysicalDevice,11),&device.memory);
    VulkanRender *render=vulkan_render_create(&device,error);
    if(is("pass-failure") || is("missing-render-entry")) {assert(!render && !passes && error[0]); return 0;}
    assert(render && passes==VULKAN_COLOR_COUNT); assert(!vulkan_render_target(render,4097,1,error));
    if(is("depth")) {
        VulkanTarget *depth=vulkan_render_depth_target(render,16,8,error); assert(depth && !framebuffers);
        unsigned char data[16*8*5];
        assert(!vulkan_render_clear_depth(render,depth,0.25f,true,91,3,data,sizeof data,error));
        assert(vulkan_render_clear_depth(render,depth,0.25f,true,91,255,data,sizeof data,error));
        assert(vulkan_render_clear_depth(render,depth,0.75f,true,0,0,data,sizeof data,error));
        for(size_t i=512;i<sizeof data;i++) assert(data[i]==91);
        uint32_t before; memcpy(&before,data,4);
        assert(vulkan_render_clear_depth(render,depth,0,false,17,255,data,sizeof data,error));
        uint32_t after; memcpy(&after,data,4); assert(before==after && data[512]==17);
        assert(vulkan_render_close(render)); assert(!passes && !images && !views && !buffers && !allocations && !pools && !fences); return 0;
    }
    VulkanTarget *target=vulkan_render_target(render,16,8,error);
    if(is("view-failure") || is("framebuffer-failure")) {assert(!target && !images && !views && !allocations); assert(vulkan_render_close(render)); return 0;}
    assert(target && images==1 && views==1 && framebuffers==1 && allocations==1);
    const float color[4]={1,0,1,1}; unsigned char pixels[16*8*4];
    assert(!vulkan_render_clear(render,target,color,pixels,sizeof pixels-1,error));
    assert(!vulkan_render_clear(render,target,color,pixels,sizeof pixels+4,error));
    assert(!vulkan_render_clear(render,(VulkanTarget *)1,color,pixels,sizeof pixels,error));
    bool ok=vulkan_render_clear(render,target,color,pixels,sizeof pixels,error);
    if(is("coherent") || is("noncoherent")) {
        assert(ok && !buffers && !fences && !pools && allocations==1);
        for(unsigned i=0;i<sizeof pixels;i++) assert(pixels[i]==(unsigned char)(color[i%4]*255));
        assert(invalidations==(unsigned)is("noncoherent")); assert(vulkan_render_release(render,target));
    } else {
        assert(!ok && error[0]);
        if(is("timeout") || is("submit-failure")) {
            assert(!vulkan_render_close(render) && images==1 && views==1 && framebuffers==1 && buffers==1 && allocations==2 && fences==1 && pools==1);
            assert(!vulkan_render_release(render,target)); assert(!vulkan_render_target(render,1,1,error)); allow_completion=true;
        }
    }
    assert(vulkan_render_close(render));
    assert(!passes && !images && !views && !framebuffers && !buffers && !allocations && !pools && !fences); return 0;
}
