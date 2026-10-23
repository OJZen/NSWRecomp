#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct VulkanHost VulkanHost;
typedef struct {
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetDeviceProcAddr get_proc;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDevice physical;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties;
    bool depth_clamp;
    bool image_gather_extended,draw_parameters;
} VulkanDevice;
typedef struct {
    VkPhysicalDeviceProperties device;
    uint32_t adapter_index, queue_family;
} VulkanHostInfo;
typedef struct {
    uint32_t bytes, pattern, source_memory_type, readback_memory_type;
    bool coherent;
} VulkanTransferProof;

// The loader must stay alive until close succeeds. Adapter -1 chooses a hardware
// graphics+compute device, preferring discrete GPUs. Optional depth clamp, image
// gather and shader draw parameters are queried and enabled only when supported.
VulkanHost *vulkan_host_create(PFN_vkGetInstanceProcAddr loader, int adapter, char error[256]);
const VulkanHostInfo *vulkan_host_info(const VulkanHost *host);
// Borrowed device/queue; dependent render resources must close before the host.
bool vulkan_host_device(const VulkanHost *host, VulkanDevice *device);
// Single-owner synchronous transfer check; size is 4-byte aligned, <=16 MiB.
bool vulkan_host_transfer(VulkanHost *host, uint32_t bytes, uint32_t pattern,
                          uint64_t timeout_ns, VulkanTransferProof *proof, char error[256]);
// Never waits indefinitely. False retains all objects when a submission is still
// pending: the owning bounded worker must terminate, or close again after completion.
bool vulkan_host_close(VulkanHost *host);
