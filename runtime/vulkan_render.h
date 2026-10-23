#pragma once
#include "vulkan_host.h"
#include "vulkan_color.h"

typedef struct VulkanRender VulkanRender;
typedef struct VulkanTarget VulkanTarget;
VulkanRender *vulkan_render_create(const VulkanDevice *device, char error[256]);
VulkanTarget *vulkan_render_target(VulkanRender *render, uint32_t width, uint32_t height, char error[256]);
VulkanTarget *vulkan_render_target_format(VulkanRender *render, uint32_t width, uint32_t height, VulkanColorFormat format, char error[256]);
VulkanTarget *vulkan_render_depth_target(VulkanRender *render, uint32_t width, uint32_t height, char error[256]);
size_t vulkan_render_target_pixels(VulkanRender *render, VulkanTarget *target);
// Execute a color-attachment render pass, optionally read and verify its packed
// pixels. Readback is exactly width*height*format bytes (R8: 1, other colors: 4).
// All calls, including transfers sharing the queue, require external serialization.
bool vulkan_render_clear(VulkanRender *render, VulkanTarget *target, const float color[4],
                         void *rgba, size_t capacity, char error[256]);
// D24S8 readback has a 32-bit depth plane followed by an 8-bit stencil plane.
// Stencil masks are 0 or 255; an unselected aspect is preserved.
bool vulkan_render_clear_depth(VulkanRender *,VulkanTarget *,float depth,bool clear_depth,
                               uint8_t stencil,uint8_t stencil_mask,void *pixels,size_t capacity,char error[256]);
bool vulkan_render_release(VulkanRender *render, VulkanTarget *target);
bool vulkan_render_copy(VulkanRender *,VulkanTarget *source,VulkanTarget *destination,bool linear,void *pixels,size_t capacity,char error[256]);
bool vulkan_render_pixels(VulkanRender *,VulkanTarget *,bool upload,void *pixels,size_t capacity,char error[256]);
// Pending GPU work retains every referenced object; terminate the bounded worker
// if close returns false. Never destroy the parent VulkanHost in that case.
bool vulkan_render_close(VulkanRender *render);
