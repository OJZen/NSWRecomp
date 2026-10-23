#include "vulkan_compute.h"
#include "vulkan_shader_checks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUNCTIONS(X) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) \
    X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)

typedef struct {VkBuffer buffer; VkDeviceMemory memory; bool coherent; void *copy;} Buffer;
struct VulkanCompute {
    VulkanDevice device;
    VkShaderModule module;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorPool descriptors;
    VkCommandPool pool;
    VkFence fence;
    Buffer buffers[32];
    bool pending;
#define DECLARE(n) PFN_vk##n n;
    FUNCTIONS(DECLARE)
#undef DECLARE
};
static void clear(VulkanCompute *c) {
    VkDevice d=c->device.device;
    if(c->fence) c->DestroyFence(d,c->fence,NULL);
    if(c->pool) c->DestroyCommandPool(d,c->pool,NULL);
    if(c->descriptors) c->DestroyDescriptorPool(d,c->descriptors,NULL);
    if(c->pipeline) c->DestroyPipeline(d,c->pipeline,NULL);
    if(c->layout) c->DestroyPipelineLayout(d,c->layout,NULL);
    if(c->set_layout) c->DestroyDescriptorSetLayout(d,c->set_layout,NULL);
    if(c->module) c->DestroyShaderModule(d,c->module,NULL);
    for(unsigned i=0;i<32;i++) {
        Buffer *b=&c->buffers[i];
        if(b->buffer) c->DestroyBuffer(d,b->buffer,NULL);
        if(b->memory) c->FreeMemory(d,b->memory,NULL);
        free(b->copy); *b=(Buffer){0};
    }
    c->fence=VK_NULL_HANDLE; c->pool=VK_NULL_HANDLE; c->descriptors=VK_NULL_HANDLE; c->pipeline=VK_NULL_HANDLE;
    c->layout=VK_NULL_HANDLE; c->set_layout=VK_NULL_HANDLE; c->module=VK_NULL_HANDLE;
}
VulkanCompute *vulkan_compute_create(const VulkanDevice *d,char error[256]) {
    if(!d || !d->device || !d->queue || !d->get_proc) {snprintf(error,256,"invalid compute device"); return NULL;}
    VulkanCompute *c=calloc(1,sizeof *c); if(!c) {snprintf(error,256,"compute context allocation failed"); return NULL;}
    c->device=*d;
#define LOAD(n) c->n=(PFN_vk##n)d->get_proc(d->device,"vk" #n); if(!c->n) {snprintf(error,256,"missing compute entry vk%s",#n); free(c); return NULL;}
    FUNCTIONS(LOAD)
#undef LOAD
    return c;
}
bool vulkan_compute_close(VulkanCompute *c) {
    if(!c) return true;
    if(c->pending && c->WaitForFences(c->device.device,1,&c->fence,VK_TRUE,0)!=VK_SUCCESS) return false;
    clear(c); free(c); return true;
}
#define CHECK(call) do {VkResult result=(call); if(result!=VK_SUCCESS) {snprintf(error,256,"%s: VkResult %d",#call,(int)result); goto fail;}} while(0)
bool vulkan_compute_dispatch(VulkanCompute *c,const uint32_t *spirv,size_t bytes,const uint32_t groups[3],
                             const VulkanComputeBuffer *buffers,unsigned count,char error[256]) {
    if(!c || c->pending || !spirv || bytes<20 || bytes>(4u<<20) || bytes%4 || spirv[0]!=0x07230203 || !groups || !buffers || !count || count>32) {
        snprintf(error,256,"invalid compute shader, bindings or pending work"); return false;
    }
    uint64_t invocations=1;
    if(!vulkan_shader_supported(&c->device,spirv,bytes,5,error)) return false;
    for(unsigned i=0;i<3;i++) {if(!groups[i] || groups[i]>65535) {snprintf(error,256,"invalid compute group extent"); return false;} invocations*=groups[i];}
    if(invocations>(1u<<20)) {snprintf(error,256,"compute group count exceeds bound"); return false;}
    size_t total=0;
    VkDescriptorSetLayoutBinding bindings[32]; VkDescriptorBufferInfo info[32]; VkWriteDescriptorSet writes[32];
    for(unsigned i=0;i<count;i++) {
        const VulkanComputeBuffer *b=&buffers[i];
        if(!b->input || !b->size || b->size>(16u<<20) || (!b->storage && (b->size>65536 || b->output)) || b->binding>=128 || b->size>(64u<<20)-total) {
            snprintf(error,256,"compute buffer range exceeds bound"); return false;
        }
        total+=b->size;
        for(unsigned j=0;j<i;j++) if(buffers[j].binding==b->binding) {snprintf(error,256,"duplicate compute binding"); return false;}
        bindings[i]=(VkDescriptorSetLayoutBinding){.binding=b->binding,.descriptorType=b->storage?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDevice d=c->device.device;
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=spirv};
    CHECK(c->CreateShaderModule(d,&shader,NULL,&c->module));
    VkDescriptorSetLayoutCreateInfo set_layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=bindings};
    CHECK(c->CreateDescriptorSetLayout(d,&set_layout,NULL,&c->set_layout));
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&c->set_layout};
    CHECK(c->CreatePipelineLayout(d,&layout,NULL,&c->layout));
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=c->layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=c->module,.pName="main"}};
    CHECK(c->CreateComputePipelines(d,VK_NULL_HANDLE,1,&pipeline,NULL,&c->pipeline));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count}};
    VkDescriptorPoolCreateInfo descriptors={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    CHECK(c->CreateDescriptorPool(d,&descriptors,NULL,&c->descriptors));
    VkDescriptorSet set; VkDescriptorSetAllocateInfo set_alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=c->descriptors,.descriptorSetCount=1,.pSetLayouts=&c->set_layout};
    CHECK(c->AllocateDescriptorSets(d,&set_alloc,&set));
    VkDeviceSize allocated=0;
    for(unsigned i=0;i<count;i++) {
        Buffer *b=&c->buffers[i]; const VulkanComputeBuffer *input=&buffers[i];
        VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=input->size,
            .usage=input->storage?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(c->CreateBuffer(d,&buffer,NULL,&b->buffer));
        VkMemoryRequirements req; c->GetBufferMemoryRequirements(d,b->buffer,&req);
        if(!req.size || req.size>(128u<<20)-allocated) {snprintf(error,256,"compute allocation exceeds bound"); goto fail;} allocated+=req.size;
        uint32_t type=UINT32_MAX;
        for(unsigned j=0;j<c->device.memory.memoryTypeCount;j++) {
            VkMemoryPropertyFlags flags=c->device.memory.memoryTypes[j].propertyFlags;
            if((req.memoryTypeBits&(1u<<j)) && (flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type=j; if(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
            }
        }
        if(type==UINT32_MAX) {snprintf(error,256,"no host-visible compute memory"); goto fail;}
        b->coherent=!!(c->device.memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo memory={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
        CHECK(c->AllocateMemory(d,&memory,NULL,&b->memory)); CHECK(c->BindBufferMemory(d,b->buffer,b->memory,0));
        void *mapped; CHECK(c->MapMemory(d,b->memory,0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,input->input,input->size);
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b->memory,.size=VK_WHOLE_SIZE};
        VkResult flushed=b->coherent?VK_SUCCESS:c->FlushMappedMemoryRanges(d,1,&range); c->UnmapMemory(d,b->memory); CHECK(flushed);
        if(input->output) {b->copy=malloc(input->size); if(!b->copy) {snprintf(error,256,"compute readback allocation failed"); goto fail;}}
        info[i]=(VkDescriptorBufferInfo){b->buffer,0,input->size};
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=input->binding,.descriptorCount=1,.descriptorType=bindings[i].descriptorType,.pBufferInfo=&info[i]};
    }
    c->UpdateDescriptorSets(d,count,writes,0,NULL);
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=c->device.queue_family};
    CHECK(c->CreateCommandPool(d,&pool,NULL,&c->pool));
    VkCommandBuffer command; VkCommandBufferAllocateInfo command_alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=c->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(c->AllocateCommandBuffers(d,&command_alloc,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(c->BeginCommandBuffer(command,&begin));
    VkMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    c->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,NULL,0,NULL);
    c->CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,c->pipeline);
    c->CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,c->layout,0,1,&set,0,NULL);
    c->CmdDispatch(command,groups[0],groups[1],groups[2]);
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    c->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,NULL,0,NULL);
    CHECK(c->EndCommandBuffer(command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CHECK(c->CreateFence(d,&fence,NULL,&c->fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    // A failed submit may have partially submitted work. Retain every object.
    c->pending=true; CHECK(c->QueueSubmit(c->device.queue,1,&submit,c->fence));
    CHECK(c->WaitForFences(d,1,&c->fence,VK_TRUE,5000000000ull)); c->pending=false;
    for(unsigned i=0;i<count;i++) if(buffers[i].output) {
        Buffer *b=&c->buffers[i]; void *mapped; CHECK(c->MapMemory(d,b->memory,0,VK_WHOLE_SIZE,0,&mapped));
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b->memory,.size=VK_WHOLE_SIZE};
        VkResult invalidated=b->coherent?VK_SUCCESS:c->InvalidateMappedMemoryRanges(d,1,&range);
        if(invalidated==VK_SUCCESS) memcpy(b->copy,mapped,buffers[i].size); c->UnmapMemory(d,b->memory); CHECK(invalidated);
    }
    for(unsigned i=0;i<count;i++) if(buffers[i].output) memcpy(buffers[i].output,c->buffers[i].copy,buffers[i].size);
    clear(c); return true;
fail:
    if(!c->pending) clear(c); return false;
}
#undef CHECK
