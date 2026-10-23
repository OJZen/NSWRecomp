#pragma once
#include "vulkan_render.h"
typedef struct {uint32_t set,binding; const void *data; size_t size;} VulkanDrawUniform;
typedef struct {uint32_t set,binding; VulkanTarget *target; VkSamplerCreateInfo sampler;} VulkanDrawTexture;
typedef struct {const void *data; size_t size; uint32_t stride;} VulkanDrawVertex;
typedef struct {
    VulkanTarget *color,*depth;
    VkPipelineDepthStencilStateCreateInfo depth_state;
    const uint32_t *vertex_shader,*fragment_shader; size_t vertex_shader_bytes,fragment_shader_bytes;
    const VulkanDrawUniform *uniforms; unsigned uniform_count;
    const VulkanDrawTexture *textures; unsigned texture_count;
    VulkanDrawVertex vertices[16];
    VkVertexInputAttributeDescription attributes[16]; unsigned attribute_count;
    const void *indices; size_t index_bytes; VkIndexType index_type;
    uint32_t count,instances,first,base_instance; int32_t base_vertex;
    VkPrimitiveTopology topology;
    VkViewport viewport; VkRect2D scissor;
    VkCullModeFlags cull; VkFrontFace front;
    bool depth_clamp;
    VkPipelineColorBlendAttachmentState blend;
    float blend_constants[4]; VkSampleMask sample_mask;
} VulkanDraw;
// One color attachment, one viewport, triangle/strip/fan topology, uniform buffers
// and combined 2D samplers. Unsupported state is rejected before submission.
bool vulkan_render_draw(VulkanRender *,const VulkanDraw *,void *pixels,size_t capacity,char error[256]);
