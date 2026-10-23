#include "vulkan_render.h"
#include "vulkan_draw.h"
#include "vulkan_shader_checks.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUNCTIONS(X) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CmdCopyImageToBuffer) \
    X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CmdClearDepthStencilImage) X(CmdCopyImage) X(CmdBlitImage) \
    X(CmdCopyBufferToImage) X(FlushMappedMemoryRanges)

struct VulkanTarget {
    struct VulkanTarget *next;
    uint32_t width, height;
    VulkanColorFormat format;
    bool depth;
    VkImageLayout layout;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkDeviceSize allocation_size;
};
struct VulkanRender {
    VulkanDevice device;
    VulkanTarget *targets;
    VkRenderPass pass[VULKAN_COLOR_COUNT];
    VkCommandPool pool;
    VkFence fence;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    bool pending;
    unsigned target_count;
    VkDeviceSize target_bytes;
    struct VulkanDrawWork *draw;
#define DECLARE(name) PFN_vk##name name;
    FUNCTIONS(DECLARE)
#undef DECLARE
};
static void draw_destroy(VulkanRender *r);

static bool failure(char error[256], const char *operation, VkResult result) {
    snprintf(error, 256, "%s: VkResult %d", operation, (int)result); return false;
}
static const VkFormat formats[VULKAN_COLOR_COUNT]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT};
static void work_destroy(VulkanRender *r) {
    VkDevice device = r->device.device;
    if (r->fence) r->DestroyFence(device, r->fence, NULL);
    if (r->pool) r->DestroyCommandPool(device, r->pool, NULL);
    if (r->readback) r->DestroyBuffer(device, r->readback, NULL);
    if (r->readback_memory) r->FreeMemory(device, r->readback_memory, NULL);
    r->fence = VK_NULL_HANDLE; r->pool = VK_NULL_HANDLE;
    r->readback = VK_NULL_HANDLE; r->readback_memory = VK_NULL_HANDLE;
    draw_destroy(r);
}
static void target_destroy(VulkanRender *r, VulkanTarget *target) {
    VkDevice device = r->device.device;
    if (target->framebuffer) r->DestroyFramebuffer(device, target->framebuffer, NULL);
    if (target->view) r->DestroyImageView(device, target->view, NULL);
    if (target->image) r->DestroyImage(device, target->image, NULL);
    if (target->memory) r->FreeMemory(device, target->memory, NULL);
    free(target);
}
bool vulkan_render_release(VulkanRender *r, VulkanTarget *target) {
    if (!r || r->pending || !target) return false;
    VulkanTarget **item = &r->targets;
    while (*item && *item != target) item = &(*item)->next;
    if (!*item) return false;
    *item = target->next; r->target_count--; r->target_bytes -= target->allocation_size;
    target_destroy(r, target); return true;
}
bool vulkan_render_close(VulkanRender *r) {
    if (!r) return true;
    if (r->pending) {
        if (r->WaitForFences(r->device.device, 1, &r->fence, VK_TRUE, 0) != VK_SUCCESS) return false;
        r->pending = false;
    }
    work_destroy(r);
    while (r->targets) vulkan_render_release(r, r->targets);
    for(unsigned i=0;i<VULKAN_COLOR_COUNT;i++) if (r->pass[i]) r->DestroyRenderPass(r->device.device, r->pass[i], NULL);
    free(r); return true;
}

VulkanRender *vulkan_render_create(const VulkanDevice *device, char error[256]) {
    if (!device || !device->device || !device->queue || !device->get_proc) { snprintf(error, 256, "invalid render device"); return NULL; }
    VulkanRender *r = calloc(1, sizeof *r);
    if (!r) { snprintf(error, 256, "render allocation failed"); return NULL; }
    r->device = *device;
#define LOAD(name) r->name = (PFN_vk##name)device->get_proc(device->device, "vk" #name); \
    if (!r->name) { snprintf(error, 256, "missing render entry vk%s", #name); free(r); return NULL; }
    FUNCTIONS(LOAD)
#undef LOAD
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT},
    };
    VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 2, .pDependencies = dependencies};
    for(unsigned i=0;i<VULKAN_COLOR_COUNT;i++) {
        attachment.format=formats[i];
        VkResult result = r->CreateRenderPass(device->device, &pass, NULL, &r->pass[i]);
        if (result != VK_SUCCESS) { failure(error, "vkCreateRenderPass", result); vulkan_render_close(r); return NULL; }
    }
    return r;
}
static bool allocate(VulkanRender *r, VkMemoryRequirements requirements, bool host, VkDeviceMemory *out, bool *coherent, char error[256]) {
    uint32_t type = UINT32_MAX; int score = -1;
    if (!requirements.size || requirements.size > (128u << 20)) { snprintf(error, 256, "render allocation exceeds bound"); return false; }
    for (uint32_t i = 0; i < r->device.memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = r->device.memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) || (host && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) continue;
        int preference = !!(flags & (host ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        if (preference > score) { score = preference; type = i; }
    }
    if (type == UINT32_MAX) { snprintf(error, 256, "no compatible render memory"); return false; }
    VkMemoryAllocateInfo memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    VkResult result = r->AllocateMemory(r->device.device, &memory, NULL, out);
    if (coherent) *coherent = !!(r->device.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return result == VK_SUCCESS || failure(error, "vkAllocateMemory", result);
}
#define CHECK(operation) do { VkResult result = (operation); if (result != VK_SUCCESS) { failure(error, #operation, result); goto fail; } } while (0)

VulkanTarget *vulkan_render_target(VulkanRender *r, uint32_t width, uint32_t height, char error[256]) {
    return vulkan_render_target_format(r,width,height,VULKAN_COLOR_RGBA8,error);
}
static VulkanTarget *target_create(VulkanRender *r, uint32_t width, uint32_t height, VulkanColorFormat format,bool depth,char error[256]) {
    if (!r || r->pending || !width || !height || width > 4096 || height > 4096 || r->target_count >= 64 || (unsigned)format>=VULKAN_COLOR_COUNT) { snprintf(error, 256, "invalid render extent, format, target limit or pending work"); return NULL; }
    VulkanTarget *target = calloc(1, sizeof *target);
    if (!target) { snprintf(error, 256, "target allocation failed"); return NULL; }
    target->width = width; target->height = height; target->format=format; target->depth=depth; VkDevice device = r->device.device;
    VkFormat vk_format=depth?VK_FORMAT_D24_UNORM_S8_UINT:formats[format];
    VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = (depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(r->CreateImage(device, &image, NULL, &target->image));
    VkMemoryRequirements requirements; r->GetImageMemoryRequirements(device, target->image, &requirements);
    if (requirements.size > (256u << 20) - r->target_bytes) { snprintf(error, 256, "render targets exceed 256 MiB bound"); goto fail; }
    if (!allocate(r, requirements, false, &target->memory, NULL, error)) goto fail;
    CHECK(r->BindImageMemory(device, target->image, target->memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = target->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vk_format,
        .subresourceRange = {.aspectMask = depth?(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
    CHECK(r->CreateImageView(device, &view, NULL, &target->view));
    VkFramebufferCreateInfo framebuffer = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r->pass[format],
        .attachmentCount = 1, .pAttachments = &target->view, .width = width, .height = height, .layers = 1};
    if(!depth) CHECK(r->CreateFramebuffer(device, &framebuffer, NULL, &target->framebuffer));
    target->next = r->targets; r->targets = target;
    target->allocation_size = requirements.size; r->target_bytes += requirements.size; r->target_count++;
    return target;
fail:
    target_destroy(r, target); return NULL;
}
VulkanTarget *vulkan_render_target_format(VulkanRender *r,uint32_t width,uint32_t height,VulkanColorFormat format,char error[256]) {
    return target_create(r,width,height,format,false,error);
}
VulkanTarget *vulkan_render_depth_target(VulkanRender *r,uint32_t width,uint32_t height,char error[256]) {
    return target_create(r,width,height,VULKAN_COLOR_RGBA8,true,error);
}
size_t vulkan_render_target_pixels(VulkanRender *r,VulkanTarget *target) {
    if(r) for(VulkanTarget *t=r->targets;t;t=t->next) if(t==target) return (size_t)t->width*t->height;
    return 0;
}
static bool clear_target(VulkanRender *r,VulkanTarget *target,const float color[4],const VkClearDepthStencilValue *ds,
                        VkImageAspectFlags clear_aspects,void *rgba,size_t capacity,char error[256]) {
    if (!r || !target || (!color && !ds) || r->pending) { snprintf(error, 256, "invalid clear request or pending submission"); return false; }
    VulkanTarget *item = r->targets; while (item && item != target) item = item->next;
    if (!item || target->depth!=(ds!=NULL)) { snprintf(error, 256, "invalid clear target format"); return false; }
    unsigned bpp=ds?5:vulkan_color_bytes(target->format);
    size_t bytes = (size_t)target->width * target->height * bpp;
    if ((rgba && capacity != bytes) || (!rgba && capacity)) { snprintf(error, 256, "invalid readback capacity"); return false; }
    if(color) for (unsigned i = 0; i < 4; i++) if (!isfinite(color[i]) || color[i] < 0 || color[i] > (target->format==VULKAN_COLOR_RGB11F && i<3?64512:1)) { snprintf(error, 256, "invalid clear color"); return false; }
    VkDevice device = r->device.device; bool coherent = false;
    if (rgba) {
        VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(r->CreateBuffer(device, &buffer, NULL, &r->readback));
        VkMemoryRequirements requirements; r->GetBufferMemoryRequirements(device, r->readback, &requirements);
        if (!allocate(r, requirements, true, &r->readback_memory, &coherent, error)) goto fail;
        CHECK(r->BindBufferMemory(device, r->readback, r->readback_memory, 0));
    }
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = r->device.queue_family};
    CHECK(r->CreateCommandPool(device, &pool, NULL, &r->pool));
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(r->AllocateCommandBuffers(device, &allocation, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(r->BeginCommandBuffer(command, &begin));
    if(ds) {
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=target->layout?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=target->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=target->image,
            .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.levelCount=1,.layerCount=1}};
        r->CmdPipelineBarrier(command,target->layout?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkImageSubresourceRange range={.aspectMask=clear_aspects,.levelCount=1,.layerCount=1};
        r->CmdClearDepthStencilImage(command,target->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,ds,1,&range);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    } else {
        VkClearValue clear; memcpy(clear.color.float32, color, 16);
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->pass[target->format],
            .framebuffer = target->framebuffer, .renderArea = {.extent = {target->width, target->height}}, .clearValueCount = 1, .pClearValues = &clear};
        r->CmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        r->CmdEndRenderPass(command);
    }
    if (rgba) {
        VkBufferImageCopy copy = {.imageSubresource = {.aspectMask = ds?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .imageExtent = {target->width, target->height, 1}};
        r->CmdCopyImageToBuffer(command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->readback, 1, &copy);
        if(ds) {copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT; copy.bufferOffset=(VkDeviceSize)target->width*target->height*4;
            r->CmdCopyImageToBuffer(command,target->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->readback,1,&copy);}
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        r->CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    }
    CHECK(r->EndCommandBuffer(command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(r->CreateFence(device, &fence, NULL, &r->fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    r->pending = true;
    CHECK(r->QueueSubmit(r->device.queue, 1, &submit, r->fence));
    CHECK(r->WaitForFences(device, 1, &r->fence, VK_TRUE, UINT64_C(5000000000)));
    r->pending = false;
    target->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (rgba) {
        void *mapped; CHECK(r->MapMemory(device, r->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        if (!coherent) {
            VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = r->readback_memory, .size = VK_WHOLE_SIZE};
            VkResult result = r->InvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) { r->UnmapMemory(device, r->readback_memory); failure(error, "vkInvalidateMappedMemoryRanges", result); goto fail; }
        }
        memcpy(rgba, mapped, bytes); r->UnmapMemory(device, r->readback_memory);
        size_t count=(size_t)target->width*target->height;
        for(size_t pixel=0;pixel<count;pixel++) {
            bool matches;
            if(ds) {
                uint32_t depth; memcpy(&depth,(const unsigned char *)rgba+pixel*4,4);
                uint32_t expected=(uint32_t)llround((double)ds->depth*16777215);
                matches=(!(clear_aspects&VK_IMAGE_ASPECT_DEPTH_BIT) || llabs((int64_t)(depth&0xffffff)-expected)<=1) &&
                    (!(clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT) || ((const unsigned char *)rgba)[count*4+pixel]==ds->stencil);
            } else matches=vulkan_color_matches(target->format,(const unsigned char *)rgba+pixel*bpp,color);
            if(!matches) {snprintf(error,256,"attachment clear readback mismatch"); goto fail;}
        }
    }
    target->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; work_destroy(r); return true;
fail:
    if (!r->pending) work_destroy(r);
    return false;
}
bool vulkan_render_clear(VulkanRender *r,VulkanTarget *target,const float color[4],void *pixels,size_t capacity,char error[256]) {
    return clear_target(r,target,color,NULL,0,pixels,capacity,error);
}
bool vulkan_render_clear_depth(VulkanRender *r,VulkanTarget *target,float depth,bool clear_depth,
    uint8_t stencil,uint8_t stencil_mask,void *pixels,size_t capacity,char error[256]) {
    if((!clear_depth && !stencil_mask) || (stencil_mask && stencil_mask!=255) ||
        (clear_depth && (!isfinite(depth) || depth<0 || depth>1))) {snprintf(error,256,"unsupported depth/stencil clear mask or value"); return false;}
    VkClearDepthStencilValue value={clear_depth?depth:0,stencil};
    return clear_target(r,target,NULL,&value,(clear_depth?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(stencil_mask?VK_IMAGE_ASPECT_STENCIL_BIT:0),pixels,capacity,error);
}
#include "vulkan_pixels.inc"
#include "vulkan_image_io.inc"
#include "vulkan_draw.inc"
#undef CHECK
