#include "vulkan_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceFeatures2) \
    X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdFillBuffer) X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) \
    X(QueueSubmit) X(WaitForFences)

struct VulkanHost {
    VulkanHostInfo info;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkBuffer buffers[2];
    VkDeviceMemory allocations[2];
    VkCommandPool pool;
    VkFence fence;
    bool pending;
    bool depth_clamp,image_gather_extended,draw_parameters;
#define DECLARE(name) PFN_vk##name name;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
};

static bool error_result(char error[256], const char *operation, VkResult result) {
    snprintf(error, 256, "%s: VkResult %d", operation, (int)result);
    return false;
}

static void transfer_destroy(VulkanHost *h) {
    if (h->fence) h->DestroyFence(h->device, h->fence, NULL);
    if (h->pool) h->DestroyCommandPool(h->device, h->pool, NULL);
    for (unsigned i = 0; i < 2; i++) {
        if (h->buffers[i]) h->DestroyBuffer(h->device, h->buffers[i], NULL);
        if (h->allocations[i]) h->FreeMemory(h->device, h->allocations[i], NULL);
        h->buffers[i] = VK_NULL_HANDLE; h->allocations[i] = VK_NULL_HANDLE;
    }
    h->fence = VK_NULL_HANDLE; h->pool = VK_NULL_HANDLE;
}

bool vulkan_host_close(VulkanHost *h) {
    if (!h) return true;
    if (h->pending) {
        if (h->WaitForFences(h->device, 1, &h->fence, VK_TRUE, 0) != VK_SUCCESS) return false;
        h->pending = false;
    }
    transfer_destroy(h);
    if (h->device && h->DestroyDevice) h->DestroyDevice(h->device, NULL);
    if (h->instance && h->DestroyInstance) h->DestroyInstance(h->instance, NULL);
    free(h);
    return true;
}

VulkanHost *vulkan_host_create(PFN_vkGetInstanceProcAddr loader, int adapter, char error[256]) {
    error[0] = 0;
    if (!loader || adapter < -1 || adapter >= 32) {
        snprintf(error, 256, "invalid Vulkan loader or adapter index"); return NULL;
    }
    PFN_vkEnumerateInstanceVersion version_fn = (PFN_vkEnumerateInstanceVersion)loader(NULL, "vkEnumerateInstanceVersion");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)loader(NULL, "vkCreateInstance");
    uint32_t version = 0;
    if (!version_fn || !create || version_fn(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
        snprintf(error, 256, "Vulkan 1.3 loader required"); return NULL;
    }
    VulkanHost *h = calloc(1, sizeof *h);
    if (!h) { snprintf(error, 256, "Vulkan host allocation failed"); return NULL; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "NSWRecomp",
        .pEngineName = "NSWRecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkResult result = create(&instance, NULL, &h->instance);
    if (result != VK_SUCCESS) { error_result(error, "vkCreateInstance", result); goto fail; }
#define LOAD_INSTANCE(name) \
    h->name = (PFN_vk##name)loader(h->instance, "vk" #name); \
    if (!h->name) { snprintf(error, 256, "missing Vulkan entry: vk%s", #name); goto fail; }
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    VkPhysicalDevice devices[32]; uint32_t count = 32;
    result = h->EnumeratePhysicalDevices(h->instance, &count, devices);
    if (result != VK_SUCCESS || count > 32) { error_result(error, "vkEnumeratePhysicalDevices", result); goto fail; }
    int best_score = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (adapter >= 0 && (uint32_t)adapter != i) continue;
        VkPhysicalDeviceProperties properties;
        h->GetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 ||
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        VkQueueFamilyProperties queues[64]; uint32_t queue_count = 0;
        h->GetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, NULL);
        if (!queue_count || queue_count > 64) continue;
        h->GetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; q++) {
            VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (!queues[q].queueCount || (queues[q].queueFlags & required) != required) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                        properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best_score) {
                h->physical = devices[i]; best_score = score;
                h->info = (VulkanHostInfo){properties, i, q};
            }
            break;
        }
    }
    if (!h->physical) { snprintf(error, 256, "no eligible Vulkan 1.3 hardware graphics+compute adapter"); goto fail; }
    h->GetPhysicalDeviceMemoryProperties(h->physical, &h->memory);
    VkPhysicalDeviceVulkan11Features supported11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 supported={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&supported11}; h->GetPhysicalDeviceFeatures2(h->physical,&supported);
    VkPhysicalDeviceFeatures enabled={.depthClamp=supported.features.depthClamp,.shaderImageGatherExtended=supported.features.shaderImageGatherExtended};
    VkPhysicalDeviceVulkan11Features enabled11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.shaderDrawParameters=supported11.shaderDrawParameters};
    h->depth_clamp=enabled.depthClamp!=0; h->image_gather_extended=enabled.shaderImageGatherExtended!=0; h->draw_parameters=enabled11.shaderDrawParameters!=0;
    const float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = h->info.queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,.pEnabledFeatures=&enabled,.pNext=&enabled11};
    result = h->CreateDevice(h->physical, &device, NULL, &h->device);
    if (result != VK_SUCCESS) { error_result(error, "vkCreateDevice", result); goto fail; }
#define LOAD_DEVICE(name) \
    h->name = (PFN_vk##name)h->GetDeviceProcAddr(h->device, "vk" #name); \
    if (!h->name) { snprintf(error, 256, "missing Vulkan entry: vk%s", #name); goto fail; }
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    h->GetDeviceQueue(h->device, h->info.queue_family, 0, &h->queue);
    if (!h->queue) { snprintf(error, 256, "Vulkan device returned no queue"); goto fail; }
    return h;
fail:
    vulkan_host_close(h);
    return NULL;
}

const VulkanHostInfo *vulkan_host_info(const VulkanHost *h) { return &h->info; }
bool vulkan_host_device(const VulkanHost *h, VulkanDevice *device) {
    if (!h || !device || !h->device || h->pending) return false;
    *device = (VulkanDevice){.device=h->device,.queue=h->queue,.queue_family=h->info.queue_family,.get_proc=h->GetDeviceProcAddr,.memory=h->memory,
        .physical=h->physical,.format_properties=h->GetPhysicalDeviceFormatProperties,.depth_clamp=h->depth_clamp,.image_gather_extended=h->image_gather_extended,.draw_parameters=h->draw_parameters};
    return true;
}

static bool buffer_create(VulkanHost *h, unsigned slot, uint32_t bytes, uint32_t *type, char error[256]) {
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = h->CreateBuffer(h->device, &buffer, NULL, &h->buffers[slot]);
    if (result != VK_SUCCESS) return error_result(error, "vkCreateBuffer", result);
    VkMemoryRequirements requirements;
    h->GetBufferMemoryRequirements(h->device, h->buffers[slot], &requirements);
    VkMemoryPropertyFlags required = slot ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
    VkMemoryPropertyFlags preferred = slot ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    *type = UINT32_MAX; int score = -1;
    for (uint32_t i = 0; i < h->memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = h->memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) || (flags & required) != required) continue;
        int candidate = !!(flags & preferred);
        if (candidate > score) { score = candidate; *type = i; }
    }
    if (*type == UINT32_MAX) { snprintf(error, 256, "no compatible %s buffer memory", slot ? "readback" : "source"); return false; }
    if (requirements.size < bytes || requirements.size > (64u << 20)) {
        snprintf(error, 256, "transfer allocation exceeds the 64 MiB bound"); return false;
    }
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = *type};
    result = h->AllocateMemory(h->device, &allocation, NULL, &h->allocations[slot]);
    if (result != VK_SUCCESS) return error_result(error, "vkAllocateMemory", result);
    result = h->BindBufferMemory(h->device, h->buffers[slot], h->allocations[slot], 0);
    return result == VK_SUCCESS || error_result(error, "vkBindBufferMemory", result);
}

bool vulkan_host_transfer(VulkanHost *h, uint32_t bytes, uint32_t pattern,
                          uint64_t timeout_ns, VulkanTransferProof *proof, char error[256]) {
    error[0] = 0;
    if (!h || !proof || !bytes || (bytes & 3) || bytes > (16u << 20) ||
        !timeout_ns || timeout_ns > 5000000000ull || h->pending) {
        snprintf(error, 256, "invalid transfer request or pending submission"); return false;
    }
    uint32_t types[2];
    if (!buffer_create(h, 0, bytes, &types[0], error) || !buffer_create(h, 1, bytes, &types[1], error)) goto fail;
#define CHECK(operation) do { VkResult result = (operation); \
    if (result != VK_SUCCESS) { error_result(error, #operation, result); goto fail; } } while (0)
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = h->info.queue_family};
    CHECK(h->CreateCommandPool(h->device, &pool, NULL, &h->pool));
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = h->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(h->AllocateCommandBuffers(h->device, &allocate, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(h->BeginCommandBuffer(command, &begin));
    h->CmdFillBuffer(command, h->buffers[0], 0, bytes, pattern);
    VkMemoryBarrier copy_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    h->CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        1, &copy_barrier, 0, NULL, 0, NULL);
    VkBufferCopy copy = {.size = bytes};
    h->CmdCopyBuffer(command, h->buffers[0], h->buffers[1], 1, &copy);
    VkMemoryBarrier host_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    h->CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
        1, &host_barrier, 0, NULL, 0, NULL);
    CHECK(h->EndCommandBuffer(command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(h->CreateFence(h->device, &fence, NULL, &h->fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    h->pending = true; // Preserve every referenced object on submit/wait failure.
    CHECK(h->QueueSubmit(h->queue, 1, &submit, h->fence));
    CHECK(h->WaitForFences(h->device, 1, &h->fence, VK_TRUE, timeout_ns));
    h->pending = false;
    void *mapped = NULL;
    CHECK(h->MapMemory(h->device, h->allocations[1], 0, VK_WHOLE_SIZE, 0, &mapped));
    bool coherent = !!(h->memory.memoryTypes[types[1]].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!coherent) {
        // Offset zero and VK_WHOLE_SIZE include complete nonCoherentAtomSize units.
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = h->allocations[1], .size = VK_WHOLE_SIZE};
        VkResult result = h->InvalidateMappedMemoryRanges(h->device, 1, &range);
        if (result != VK_SUCCESS) {
            h->UnmapMemory(h->device, h->allocations[1]);
            error_result(error, "vkInvalidateMappedMemoryRanges", result); goto fail;
        }
    }
    bool matches = true;
    for (uint32_t i = 0; i < bytes / 4; i++) if (((const uint32_t *)mapped)[i] != pattern) { matches = false; break; }
    h->UnmapMemory(h->device, h->allocations[1]);
    if (!matches) { snprintf(error, 256, "GPU transfer readback mismatch"); goto fail; }
    *proof = (VulkanTransferProof){bytes, pattern, types[0], types[1], coherent};
    transfer_destroy(h);
    return true;
fail:
    if (!h->pending) transfer_destroy(h);
    return false;
#undef CHECK
}
