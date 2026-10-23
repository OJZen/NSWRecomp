// Fake Vulkan driver: deterministic queue suitability, non-coherent memory,
// partial creation failures and outstanding-submit ownership without GPU access.
#include "vulkan_host.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char *mode;
static unsigned instances, devices, buffers, allocations, pools, fences, invalidations, barriers;
static unsigned allocation_calls;
static bool completed, allow_completion;
static bool is(const char *value) { return !strcmp(mode, value); }
#define HANDLE(type, value) ((type)(uintptr_t)(value))
typedef struct { void *data; size_t bytes; } Memory;
typedef struct { Memory *memory; size_t bytes; } Buffer;
static Buffer *fill_buffer, *copy_buffer;
static uint32_t fill_pattern, fill_bytes;

static VkResult VKAPI_CALL fEnumerateInstanceVersion(uint32_t *version) {
    *version = is("old-loader") ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3; return VK_SUCCESS;
}
static VkResult VKAPI_CALL fCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    assert(ci->pApplicationInfo->apiVersion == VK_API_VERSION_1_3 && !ci->enabledExtensionCount);
    *out = HANDLE(VkInstance, 1); instances++; return VK_SUCCESS;
}
static void VKAPI_CALL fDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) { assert(!devices); instances--; }
static VkResult VKAPI_CALL fEnumeratePhysicalDevices(VkInstance i, uint32_t *count, VkPhysicalDevice *out) {
    assert(*count >= 2); *count = 2; out[0] = HANDLE(VkPhysicalDevice, 10); out[1] = HANDLE(VkPhysicalDevice, 11); return VK_SUCCESS;
}
static void VKAPI_CALL fGetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties *out) {
    memset(out, 0, sizeof *out); out->apiVersion = VK_API_VERSION_1_3;
    out->deviceType = (uintptr_t)d == 10 ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}
static void VKAPI_CALL fGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t *count, VkQueueFamilyProperties *out) {
    *count = 1;
    if (out) { memset(out, 0, sizeof *out); out->queueCount = 1;
        out->queueFlags = VK_QUEUE_GRAPHICS_BIT | ((uintptr_t)d == 11 ? VK_QUEUE_COMPUTE_BIT : 0); }
}
static void VKAPI_CALL fGetPhysicalDeviceFeatures2(VkPhysicalDevice d,VkPhysicalDeviceFeatures2 *out) {(void)d; out->features=(VkPhysicalDeviceFeatures){.depthClamp=VK_TRUE,.shaderImageGatherExtended=VK_TRUE}; ((VkPhysicalDeviceVulkan11Features *)out->pNext)->shaderDrawParameters=VK_TRUE;}
static void VKAPI_CALL fGetPhysicalDeviceFormatProperties(VkPhysicalDevice d,VkFormat format,VkFormatProperties *out) {(void)d;(void)format; *out=(VkFormatProperties){.optimalTilingFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT};}
static void VKAPI_CALL fGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *out) {
    memset(out, 0, sizeof *out); out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[1].propertyFlags = is("no-memory") ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (is("coherent")) out->memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
static VkResult VKAPI_CALL fCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    assert((uintptr_t)d == 11 && ci->queueCreateInfoCount == 1 && ci->pEnabledFeatures && ci->pEnabledFeatures->depthClamp);
    if (is("device-failure")) return VK_ERROR_INITIALIZATION_FAILED;
    *out = HANDLE(VkDevice, 2); devices++; return VK_SUCCESS;
}
static void VKAPI_CALL fDestroyDevice(VkDevice d, const VkAllocationCallbacks *a) {
    assert(!buffers && !allocations && !pools && !fences); devices--;
}
static void VKAPI_CALL fGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *out) {
    assert(!family && !index); *out = HANDLE(VkQueue, 3);
}
static VkResult VKAPI_CALL fCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    Buffer *b = calloc(1, sizeof *b); assert(b); b->bytes = ci->size;
    *out = (VkBuffer)b; buffers++; return VK_SUCCESS;
}
static void VKAPI_CALL fDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { free((Buffer *)b); buffers--; }
static void VKAPI_CALL fGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out) {
    *out = (VkMemoryRequirements){.size = ((Buffer *)b)->bytes, .alignment = 64, .memoryTypeBits = 3};
}
static VkResult VKAPI_CALL fAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ci, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    if (++allocation_calls == 2 && is("allocation-failure")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Memory *m = malloc(sizeof *m); assert(m); m->bytes = ci->allocationSize; m->data = calloc(1, m->bytes); assert(m->data);
    *out = (VkDeviceMemory)m; allocations++; return VK_SUCCESS;
}
static void VKAPI_CALL fFreeMemory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    Memory *m = (Memory *)memory; free(m->data); free(m); allocations--;
}
static VkResult VKAPI_CALL fBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
    assert(!offset); ((Buffer *)b)->memory = (Memory *)m; return VK_SUCCESS;
}
static VkResult VKAPI_CALL fMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    assert(completed && !offset && size == VK_WHOLE_SIZE);
    if (is("map-failure")) return VK_ERROR_MEMORY_MAP_FAILED;
    *out = ((Memory *)m)->data; return VK_SUCCESS;
}
static void VKAPI_CALL fUnmapMemory(VkDevice d, VkDeviceMemory m) { assert(completed); }
static VkResult VKAPI_CALL fInvalidateMappedMemoryRanges(VkDevice d, uint32_t count, const VkMappedMemoryRange *ranges) {
    assert(completed && count == 1 && !ranges->offset && ranges->size == VK_WHOLE_SIZE); invalidations++;
    return is("invalidate-failure") ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS;
}
static VkResult VKAPI_CALL fCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    *out = HANDLE(VkCommandPool, 4); pools++; return VK_SUCCESS;
}
static void VKAPI_CALL fDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { pools--; }
static VkResult VKAPI_CALL fAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ci, VkCommandBuffer *out) {
    assert(ci->commandBufferCount == 1); *out = HANDLE(VkCommandBuffer, 5); return VK_SUCCESS;
}
static VkResult VKAPI_CALL fBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *bi) {
    completed = false; barriers = 0; return VK_SUCCESS;
}
static VkResult VKAPI_CALL fEndCommandBuffer(VkCommandBuffer c) { assert(barriers == 2); return VK_SUCCESS; }
static void VKAPI_CALL fCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize offset, VkDeviceSize bytes, uint32_t pattern) {
    assert(!offset); fill_buffer = (Buffer *)b; fill_bytes = bytes; fill_pattern = pattern;
}
static void VKAPI_CALL fCmdCopyBuffer(VkCommandBuffer c, VkBuffer source, VkBuffer dest, uint32_t count, const VkBufferCopy *copy) {
    assert(barriers == 1 && (Buffer *)source == fill_buffer && count == 1 && copy->size == fill_bytes);
    copy_buffer = (Buffer *)dest;
}
static void VKAPI_CALL fCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags source, VkPipelineStageFlags dest,
    VkDependencyFlags flags, uint32_t count, const VkMemoryBarrier *memory, uint32_t bc, const VkBufferMemoryBarrier *b,
    uint32_t ic, const VkImageMemoryBarrier *i) {
    assert(count == 1 && source == VK_PIPELINE_STAGE_TRANSFER_BIT && memory->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!barriers) assert(dest == VK_PIPELINE_STAGE_TRANSFER_BIT && memory->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
    else assert(dest == VK_PIPELINE_STAGE_HOST_BIT && memory->dstAccessMask == VK_ACCESS_HOST_READ_BIT);
    barriers++;
}
static VkResult VKAPI_CALL fCreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    assert(!ci->flags); *out = HANDLE(VkFence, 6); fences++; return VK_SUCCESS;
}
static void VKAPI_CALL fDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) { assert(completed); fences--; }
static VkResult VKAPI_CALL fQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    assert(count == 1 && info->commandBufferCount == 1 && barriers == 2 && fences == 1);
    if (is("submit-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *data = fill_buffer->memory->data;
    for (unsigned i = 0; i < fill_bytes / 4; i++) data[i] = fill_pattern;
    memcpy(copy_buffer->memory->data, data, fill_bytes);
    if (is("mismatch")) ((uint32_t *)copy_buffer->memory->data)[fill_bytes / 4 - 1] ^= 1;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fWaitForFences(VkDevice d, uint32_t count, const VkFence *f, VkBool32 all, uint64_t timeout) {
    assert(count == 1 && all && timeout <= 5000000000ull);
    if ((is("timeout") || is("submit-failure")) && !allow_completion) return VK_TIMEOUT;
    completed = true; return VK_SUCCESS;
}

#define ENTRY(name) if (!strcmp(name_text, "vk" #name)) { PFN_vk##name fn = f##name; return (PFN_vkVoidFunction)fn; }
static PFN_vkVoidFunction VKAPI_CALL fGetDeviceProcAddr(VkDevice d, const char *name_text) {
    if (is("missing-entry") && !strcmp(name_text, "vkQueueSubmit")) return NULL;
    ENTRY(DestroyDevice) ENTRY(GetDeviceQueue) ENTRY(CreateBuffer) ENTRY(DestroyBuffer)
    ENTRY(GetBufferMemoryRequirements) ENTRY(AllocateMemory) ENTRY(FreeMemory) ENTRY(BindBufferMemory)
    ENTRY(MapMemory) ENTRY(UnmapMemory) ENTRY(InvalidateMappedMemoryRanges) ENTRY(CreateCommandPool)
    ENTRY(DestroyCommandPool) ENTRY(AllocateCommandBuffers) ENTRY(BeginCommandBuffer) ENTRY(EndCommandBuffer)
    ENTRY(CmdFillBuffer) ENTRY(CmdCopyBuffer) ENTRY(CmdPipelineBarrier) ENTRY(CreateFence) ENTRY(DestroyFence)
    ENTRY(QueueSubmit) ENTRY(WaitForFences)
    return NULL;
}
static PFN_vkVoidFunction VKAPI_CALL get(VkInstance i, const char *name_text) {
    ENTRY(EnumerateInstanceVersion) ENTRY(CreateInstance) ENTRY(DestroyInstance) ENTRY(EnumeratePhysicalDevices)
    ENTRY(GetPhysicalDeviceProperties) ENTRY(GetPhysicalDeviceQueueFamilyProperties) ENTRY(GetPhysicalDeviceMemoryProperties) ENTRY(GetPhysicalDeviceFormatProperties) ENTRY(GetPhysicalDeviceFeatures2)
    ENTRY(CreateDevice) ENTRY(GetDeviceProcAddr)
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2); mode = argv[1]; char error[256];
    assert(!vulkan_host_create(NULL, -1, error));
    assert(!vulkan_host_create(get, 32, error));
    VulkanHost *host = vulkan_host_create(get, is("ineligible-adapter") ? 0 : -1, error);
    if (is("old-loader") || is("device-failure") || is("missing-entry") || is("ineligible-adapter")) {
        assert(!host && error[0] && !instances && !devices); return 0;
    }
    assert(host && vulkan_host_info(host)->adapter_index == 1);
    VulkanTransferProof proof = {0};
    assert(!vulkan_host_transfer(host, 3, 0, 1, &proof, error));
    assert(!vulkan_host_transfer(host, 4, 0, 6000000000ull, &proof, error));
    bool result = vulkan_host_transfer(host, 65536, 0x89abcdef, 1000000000, &proof, error);
    if (is("coherent") || is("noncoherent")) {
        assert(result && proof.bytes == 65536 && proof.pattern == 0x89abcdef);
        assert(proof.coherent == is("coherent") && invalidations == (unsigned)!proof.coherent);
        assert(vulkan_host_transfer(host, 4, 0x76543210, 1000000000, &proof, error));
    } else {
        assert(!result && error[0] && !proof.bytes);
        if (is("timeout") || is("submit-failure")) {
            assert(!vulkan_host_close(host) && devices == 1 && buffers == 2 && fences == 1);
            assert(!vulkan_host_transfer(host, 4, 0, 1, &proof, error));
            allow_completion = true;
        }
    }
    assert(vulkan_host_close(host));
    assert(!instances && !devices && !buffers && !allocations && !pools && !fences);
    return 0;
}
