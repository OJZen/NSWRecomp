#pragma once
#include "vulkan_host.h"

typedef struct VulkanCompute VulkanCompute;
typedef struct {
    uint32_t binding;
    bool storage;
    size_t size;
    const void *input;
    void *output; // Optional storage readback; committed only after a successful fence and all mappings.
} VulkanComputeBuffer;
VulkanCompute *vulkan_compute_create(const VulkanDevice *,char error[256]);
// Synchronous, externally serialized with every other user of the device queue.
// At most 32 unique bindings and 64 MiB aggregate buffer storage per dispatch.
bool vulkan_compute_dispatch(VulkanCompute *,const uint32_t *spirv,size_t bytes,const uint32_t groups[3],
                             const VulkanComputeBuffer *,unsigned count,char error[256]);
// False retains objects referenced by pending GPU work; the owner must terminate
// its bounded worker or retry close after the fence completes.
bool vulkan_compute_close(VulkanCompute *);
