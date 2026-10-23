// Synthetic driver exercises compute ownership and transactional readback errors.
#define main transfer_driver_main
#include "native_vulkan_host.c"
#undef main
#include "vulkan_compute.h"
#include "vulkan_compute_spirv.h"
static unsigned objects,readbacks;
static bool submitted;
static Buffer *bound[2];
#define CREATE(name,Type,Info) static VkResult VKAPI_CALL c##name(VkDevice d,const Info *i,const VkAllocationCallbacks *a,Type *out) {objects++; *out=HANDLE(Type,20+objects); return VK_SUCCESS;}
#define DESTROY(name,Type) static void VKAPI_CALL c##name(VkDevice d,Type o,const VkAllocationCallbacks *a) {assert(!submitted || completed); objects--;}
CREATE(CreateShaderModule,VkShaderModule,VkShaderModuleCreateInfo)
CREATE(CreateDescriptorSetLayout,VkDescriptorSetLayout,VkDescriptorSetLayoutCreateInfo)
CREATE(CreatePipelineLayout,VkPipelineLayout,VkPipelineLayoutCreateInfo)
CREATE(CreateDescriptorPool,VkDescriptorPool,VkDescriptorPoolCreateInfo)
DESTROY(DestroyShaderModule,VkShaderModule)
DESTROY(DestroyDescriptorSetLayout,VkDescriptorSetLayout)
DESTROY(DestroyPipelineLayout,VkPipelineLayout)
DESTROY(DestroyDescriptorPool,VkDescriptorPool)
DESTROY(DestroyPipeline,VkPipeline)
static VkResult VKAPI_CALL cCreateComputePipelines(VkDevice d,VkPipelineCache cache,uint32_t n,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *out) {
    assert(n==1 && !strcmp(i->stage.pName,"main") && i->stage.stage==VK_SHADER_STAGE_COMPUTE_BIT);
    if(is("pipeline-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    objects++; *out=HANDLE(VkPipeline,30); return VK_SUCCESS;
}
static VkResult VKAPI_CALL cAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *out) {assert(i->descriptorSetCount==1); *out=HANDLE(VkDescriptorSet,31);return VK_SUCCESS;}
static void VKAPI_CALL cUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t copy,const VkCopyDescriptorSet *copies) {
    assert(n==2 && !copy);
    for(unsigned i=0;i<n;i++) {assert(w[i].dstBinding==i && w[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); bound[i]=(Buffer *)w[i].pBufferInfo->buffer;}
}
static VkResult VKAPI_CALL cMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize off,VkDeviceSize size,VkMemoryMapFlags flags,void **out) {
    assert(completed && !off && size==VK_WHOLE_SIZE);
    if(submitted && is("second-readback-failure") && ++readbacks==2) return VK_ERROR_MEMORY_MAP_FAILED;
    return fMapMemory(d,m,off,size,flags,out);
}
static VkResult VKAPI_CALL cFlushMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *r) {assert(n==1 && !r->offset && r->size==VK_WHOLE_SIZE); return is("flush-failure")?VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;}
static void VKAPI_CALL cCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint point,VkPipeline p) {assert(point==VK_PIPELINE_BIND_POINT_COMPUTE && p);}
static void VKAPI_CALL cCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint point,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t dynamic,const uint32_t *o) {assert(point==VK_PIPELINE_BIND_POINT_COMPUTE && !first && n==1 && !dynamic);}
static void VKAPI_CALL cCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z) {assert(x==4 && y==1 && z==1 && barriers==1);}
static void VKAPI_CALL cCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags source,VkPipelineStageFlags dest,VkDependencyFlags flags,uint32_t n,const VkMemoryBarrier *m,uint32_t bc,const VkBufferMemoryBarrier *b,uint32_t ic,const VkImageMemoryBarrier *i) {
    assert(n==1 && !bc && !ic);
    if(!barriers) assert(source==VK_PIPELINE_STAGE_HOST_BIT && dest==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && m->srcAccessMask==VK_ACCESS_HOST_WRITE_BIT);
    else assert(source==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && dest==VK_PIPELINE_STAGE_HOST_BIT && m->dstAccessMask==VK_ACCESS_HOST_READ_BIT);
    barriers++;
}
static VkResult VKAPI_CALL cQueueSubmit(VkQueue q,uint32_t count,const VkSubmitInfo *i,VkFence f) {
    assert(count==1 && i->commandBufferCount==1 && barriers==2); submitted=true;
    if(is("submit-failure")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for(unsigned b=0;b<2;b++) for(unsigned j=0;j<bound[b]->bytes/4;j++) ((uint32_t *)bound[b]->memory->data)[j]=((uint32_t *)bound[b]->memory->data)[j]*3+7;
    return VK_SUCCESS;
}
#define CENTRY(n) if(!strcmp(name,"vk" #n)) {PFN_vk##n f=c##n; return (PFN_vkVoidFunction)f;}
static PFN_vkVoidFunction VKAPI_CALL compute_get(VkDevice d,const char *name) {
    if(is("missing-compute-entry") && !strcmp(name,"vkCmdDispatch")) return NULL;
    CENTRY(CreateShaderModule) CENTRY(DestroyShaderModule) CENTRY(CreateDescriptorSetLayout) CENTRY(DestroyDescriptorSetLayout)
    CENTRY(CreatePipelineLayout) CENTRY(DestroyPipelineLayout) CENTRY(CreateComputePipelines) CENTRY(DestroyPipeline)
    CENTRY(CreateDescriptorPool) CENTRY(DestroyDescriptorPool) CENTRY(AllocateDescriptorSets) CENTRY(UpdateDescriptorSets)
    CENTRY(MapMemory) CENTRY(FlushMappedMemoryRanges) CENTRY(CmdBindPipeline) CENTRY(CmdBindDescriptorSets)
    CENTRY(CmdDispatch) CENTRY(CmdPipelineBarrier) CENTRY(QueueSubmit)
    return fGetDeviceProcAddr(d,name);
}
int main(int argc,char **argv) {
    assert(argc==2); mode=argv[1]; completed=true; char error[256];
    VulkanDevice device={.device=HANDLE(VkDevice,2),.queue=HANDLE(VkQueue,3),.get_proc=compute_get}; fGetPhysicalDeviceMemoryProperties(HANDLE(VkPhysicalDevice,11),&device.memory);
    VulkanCompute *compute=vulkan_compute_create(&device,error);
    if(is("missing-compute-entry")) {assert(!compute && !objects); return 0;} assert(compute);
    uint32_t input[2][32],output[2][32],groups[3]={4,1,1}; memset(output,0xaa,sizeof output);
    for(unsigned i=0;i<2;i++) for(unsigned j=0;j<32;j++) input[i][j]=i*100+j;
    VulkanComputeBuffer bindings[2]={{0,true,sizeof input[0],input[0],output[0]},{1,true,sizeof input[1],input[1],output[1]}};
    bindings[1].binding=0; assert(!vulkan_compute_dispatch(compute,synthetic_compute_spirv,sizeof synthetic_compute_spirv,groups,bindings,2,error)); bindings[1].binding=1;
    assert(!objects && !buffers && !allocations);
    bool ok=vulkan_compute_dispatch(compute,synthetic_compute_spirv,sizeof synthetic_compute_spirv,groups,bindings,2,error);
    if(is("coherent") || is("noncoherent")) {
        assert(ok); for(unsigned i=0;i<2;i++) for(unsigned j=0;j<32;j++) assert(output[i][j]==input[i][j]*3+7);
    } else {
        assert(!ok && error[0]); for(unsigned i=0;i<2;i++) for(unsigned j=0;j<32;j++) assert(output[i][j]==0xaaaaaaaa);
        if(is("timeout") || is("submit-failure")) {assert(!vulkan_compute_close(compute) && objects && buffers && allocations && pools && fences); allow_completion=true;}
    }
    assert(vulkan_compute_close(compute)); assert(!objects && !buffers && !allocations && !pools && !fences); return 0;
}
