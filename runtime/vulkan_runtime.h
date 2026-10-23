#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vulkan_color.h"
#include "vulkan_compute.h"
#include "vulkan_draw.h"

typedef struct VulkanTarget VulkanTarget;

bool vulkan_runtime_device_created(void);
bool vulkan_runtime_ready(void);
VulkanTarget *vulkan_runtime_target(uint32_t width, uint32_t height, VulkanColorFormat format, char error[256]);
VulkanTarget *vulkan_runtime_depth_target(uint32_t width,uint32_t height,char error[256]);
bool vulkan_runtime_clear(VulkanTarget *, const float color[4], void *rgba, size_t capacity, char error[256]);
bool vulkan_runtime_clear_depth(VulkanTarget *,float depth,bool clear_depth,uint8_t stencil,uint8_t stencil_mask,void *pixels,size_t capacity,char error[256]);
bool vulkan_runtime_release(VulkanTarget *);
bool vulkan_runtime_copy(VulkanTarget *source,VulkanTarget *destination,bool linear,void *pixels,size_t capacity,char error[256]);
bool vulkan_runtime_pixels(VulkanTarget *,bool upload,void *pixels,size_t capacity,char error[256]);
bool vulkan_runtime_draw(const VulkanDraw *,void *pixels,size_t capacity,char error[256]);
uint64_t vulkan_runtime_draw_count(void);
uint64_t vulkan_runtime_clear_count(void);
uint64_t vulkan_runtime_depth_clear_count(void);
uint64_t vulkan_runtime_dispatch_count(void);
bool vulkan_runtime_compute(const uint32_t *spirv,size_t bytes,const uint32_t groups[3],const VulkanComputeBuffer *,unsigned count,char error[256]);
uint64_t vulkan_runtime_readback_pixels(void);
void vulkan_runtime_shutdown(void);
