#define DRAW_FUNCTIONS(X) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateSampler) X(DestroySampler) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) \
    X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdDraw)
typedef struct VulkanDrawWork {
    VkShaderModule shaders[2]; VkDescriptorSetLayout set_layouts[2]; VkPipelineLayout layout;
    VkPipeline pipeline; VkDescriptorPool descriptors; VkRenderPass pass; VkFramebuffer framebuffer;
    VkBuffer buffers[50]; VkDeviceMemory memory[50]; unsigned count; VkDeviceSize bytes;
    VkSampler samplers[16]; VkImageView views[16];
#define DECLARE(n) PFN_vk##n n;
    DRAW_FUNCTIONS(DECLARE)
#undef DECLARE
} VulkanDrawWork;
static void draw_destroy(VulkanRender *r) {
    VulkanDrawWork *w=r->draw; if(!w) return; VkDevice d=r->device.device;
    if(w->descriptors) w->DestroyDescriptorPool(d,w->descriptors,NULL);
    if(w->pipeline) w->DestroyPipeline(d,w->pipeline,NULL);
    if(w->layout) w->DestroyPipelineLayout(d,w->layout,NULL);
    for(unsigned i=0;i<2;i++) {if(w->set_layouts[i]) w->DestroyDescriptorSetLayout(d,w->set_layouts[i],NULL); if(w->shaders[i]) w->DestroyShaderModule(d,w->shaders[i],NULL);}
    for(unsigned i=0;i<16;i++) {if(w->samplers[i]) w->DestroySampler(d,w->samplers[i],NULL); if(w->views[i]) r->DestroyImageView(d,w->views[i],NULL);}
    for(unsigned i=0;i<w->count;i++) {if(w->buffers[i]) r->DestroyBuffer(d,w->buffers[i],NULL); if(w->memory[i]) r->FreeMemory(d,w->memory[i],NULL);}
    if(w->framebuffer) r->DestroyFramebuffer(d,w->framebuffer,NULL);
    if(w->pass) r->DestroyRenderPass(d,w->pass,NULL);
    free(w); r->draw=NULL;
}
static bool draw_buffer(VulkanRender *r,const void *data,size_t bytes,VkBufferUsageFlags usage,VkBuffer *out,char error[256]) {
    VulkanDrawWork *w=r->draw; VkDevice device=r->device.device;
    if(!data || !bytes || bytes>(16u<<20) || w->count>=50) {snprintf(error,256,"draw buffer limit"); return false;}
    unsigned i=w->count++; VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(r->CreateBuffer(device,&buffer,NULL,&w->buffers[i]));
    VkMemoryRequirements requirements; r->GetBufferMemoryRequirements(device,w->buffers[i],&requirements);
    if(requirements.size>(64u<<20)-w->bytes) {snprintf(error,256,"draw allocation budget"); goto fail;} w->bytes+=requirements.size;
    bool coherent; if(!allocate(r,requirements,true,&w->memory[i],&coherent,error)) goto fail;
    CHECK(r->BindBufferMemory(device,w->buffers[i],w->memory[i],0));
    void *mapped; CHECK(r->MapMemory(device,w->memory[i],0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,data,bytes);
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=w->memory[i],.size=VK_WHOLE_SIZE};
    VkResult flushed=coherent?VK_SUCCESS:r->FlushMappedMemoryRanges(device,1,&range); r->UnmapMemory(device,w->memory[i]); CHECK(flushed);
    *out=w->buffers[i]; return true;
fail: return false;
}
bool vulkan_render_draw(VulkanRender *r,const VulkanDraw *draw,void *pixels,size_t capacity,char error[256]) {
    if(!r || r->pending || !draw || !vulkan_render_target_pixels(r,draw->color) || draw->color->depth ||
       !draw->count || !draw->instances || draw->count>(1u<<20) || draw->instances>1024 || draw->uniform_count>32 || draw->texture_count>16 || draw->attribute_count>16 ||
       (draw->depth_clamp && !r->device.depth_clamp) || draw->color->layout==VK_IMAGE_LAYOUT_UNDEFINED) {snprintf(error,256,"unsupported draw request or uninitialized target"); return false;}
    VulkanTarget *target=draw->color; size_t bytes=(size_t)target->width*target->height*vulkan_color_bytes(target->format);
    if(draw->depth && (!vulkan_render_target_pixels(r,draw->depth) || !draw->depth->depth || draw->depth->width!=target->width || draw->depth->height!=target->height ||
        !draw->depth->layout || draw->depth_state.depthWriteEnable)) {snprintf(error,256,"unsupported depth attachment or depth write"); return false;}
    if(!pixels || capacity!=bytes || bytes>(64u<<20)) {snprintf(error,256,"invalid draw readback capacity"); return false;}
    if((draw->vertex_shader_bytes|draw->fragment_shader_bytes)&3 || draw->vertex_shader_bytes<20 || draw->fragment_shader_bytes<20 ||
       draw->vertex_shader_bytes>(4u<<20) || draw->fragment_shader_bytes>(4u<<20) || !draw->vertex_shader || !draw->fragment_shader ||
       draw->vertex_shader[0]!=0x07230203 || draw->fragment_shader[0]!=0x07230203) {snprintf(error,256,"invalid draw shader"); return false;}
    if(!vulkan_shader_supported(&r->device,draw->vertex_shader,draw->vertex_shader_bytes,0,error) || !vulkan_shader_supported(&r->device,draw->fragment_shader,draw->fragment_shader_bytes,4,error)) return false;
    r->draw=calloc(1,sizeof *r->draw); if(!r->draw) {snprintf(error,256,"draw work allocation failed"); return false;}
    VulkanDrawWork *w=r->draw; VkDevice device=r->device.device;
#define LOAD(n) w->n=(PFN_vk##n)r->device.get_proc(device,"vk" #n); if(!w->n) {snprintf(error,256,"missing draw entry vk%s",#n); goto fail;}
    DRAW_FUNCTIONS(LOAD)
#undef LOAD
    for(unsigned i=0;i<2;i++) {VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=i?draw->fragment_shader_bytes:draw->vertex_shader_bytes,.pCode=i?draw->fragment_shader:draw->vertex_shader}; CHECK(w->CreateShaderModule(device,&shader,NULL,&w->shaders[i]));}
    VkDescriptorSetLayoutBinding binding[2][48]; unsigned counts[2]={0};
    VkDescriptorBufferInfo buffer_info[32]; VkDescriptorImageInfo image_info[16]; VkWriteDescriptorSet writes[48];
    for(unsigned i=0;i<draw->uniform_count;i++) {
        const VulkanDrawUniform *u=&draw->uniforms[i]; if(u->set>1 || u->binding>=128 || u->size>65536) {snprintf(error,256,"invalid draw uniform"); goto fail;}
        binding[u->set][counts[u->set]++]=(VkDescriptorSetLayoutBinding){.binding=u->binding,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
        VkBuffer b; if(!draw_buffer(r,u->data,u->size,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&b,error)) goto fail;
        buffer_info[i]=(VkDescriptorBufferInfo){b,0,u->size};
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=u->binding,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer_info[i]};
    }
    for(unsigned i=0;i<draw->texture_count;i++) {
        const VulkanDrawTexture *t=&draw->textures[i];
        if(t->set>1 || t->binding>=128 || !vulkan_render_target_pixels(r,t->target) || t->target==target || t->target->layout==VK_IMAGE_LAYOUT_UNDEFINED) {snprintf(error,256,"invalid draw texture"); goto fail;}
        binding[t->set][counts[t->set]++]=(VkDescriptorSetLayoutBinding){.binding=t->binding,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->target->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=t->target->depth?VK_FORMAT_D24_UNORM_S8_UINT:formats[t->target->format],
            .subresourceRange={.aspectMask=t->target->depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
        CHECK(r->CreateImageView(device,&view,NULL,&w->views[i])); CHECK(w->CreateSampler(device,&t->sampler,NULL,&w->samplers[i]));
        image_info[i]=(VkDescriptorImageInfo){w->samplers[i],w->views[i],t->target==draw->depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[draw->uniform_count+i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=t->binding,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image_info[i]};
    }
    for(unsigned s=0;s<2;s++) {
        for(unsigned i=0;i<counts[s];i++) for(unsigned j=0;j<i;j++) if(binding[s][i].binding==binding[s][j].binding) {snprintf(error,256,"duplicate draw descriptor"); goto fail;}
        VkDescriptorSetLayoutCreateInfo set={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=counts[s],.pBindings=binding[s]}; CHECK(w->CreateDescriptorSetLayout(device,&set,NULL,&w->set_layouts[s]));
    }
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=w->set_layouts}; CHECK(w->CreatePipelineLayout(device,&layout,NULL,&w->layout));
    VkDescriptorPoolSize pool_sizes[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,32},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16}};
    VkDescriptorPoolCreateInfo pool_desc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=2,.pPoolSizes=pool_sizes}; CHECK(w->CreateDescriptorPool(device,&pool_desc,NULL,&w->descriptors));
    VkDescriptorSet sets[2]; VkDescriptorSetAllocateInfo set_alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=w->descriptors,.descriptorSetCount=2,.pSetLayouts=w->set_layouts}; CHECK(w->AllocateDescriptorSets(device,&set_alloc,sets));
    for(unsigned i=0;i<draw->uniform_count;i++) writes[i].dstSet=sets[draw->uniforms[i].set];
    for(unsigned i=0;i<draw->texture_count;i++) writes[draw->uniform_count+i].dstSet=sets[draw->textures[i].set];
    w->UpdateDescriptorSets(device,draw->uniform_count+draw->texture_count,writes,0,NULL);
    VkBuffer vertices[16]={0},index_buffer=VK_NULL_HANDLE; VkVertexInputBindingDescription vertex_bindings[16]; unsigned vertex_count=0;
    for(unsigned i=0;i<16;i++) if(draw->vertices[i].size) {
        if(!draw_buffer(r,draw->vertices[i].data,draw->vertices[i].size,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,&vertices[i],error)) goto fail;
        vertex_bindings[vertex_count++]=(VkVertexInputBindingDescription){i,draw->vertices[i].stride,VK_VERTEX_INPUT_RATE_VERTEX};
    }
    if(draw->indices && !draw_buffer(r,draw->indices,draw->index_bytes,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,&index_buffer,error)) goto fail;
    VkAttachmentDescription attachments[2]={{.format=formats[target->format],.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format=VK_FORMAT_D24_UNORM_S8_UINT,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color,.pDepthStencilAttachment=draw->depth?&depth:NULL};
    VkSubpassDependency dependencies[2]={{.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=draw->depth?2:1,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=dependencies}; CHECK(r->CreateRenderPass(device,&pass,NULL,&w->pass));
    VkImageView attachment_views[2]={target->view,draw->depth?draw->depth->view:VK_NULL_HANDLE};
    VkFramebufferCreateInfo framebuffer={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=w->pass,.attachmentCount=draw->depth?2:1,.pAttachments=attachment_views,.width=target->width,.height=target->height,.layers=1};
    CHECK(r->CreateFramebuffer(device,&framebuffer,NULL,&w->framebuffer));
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=w->shaders[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=w->shaders[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=vertex_count,.pVertexBindingDescriptions=vertex_bindings,.vertexAttributeDescriptionCount=draw->attribute_count,.pVertexAttributeDescriptions=draw->attributes};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=draw->topology};
    VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&draw->viewport,.scissorCount=1,.pScissors=&draw->scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.depthClampEnable=draw->depth_clamp,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=draw->cull,.frontFace=draw->front,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo multi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT,.pSampleMask=&draw->sample_mask};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&draw->blend}; memcpy(blend.blendConstants,draw->blend_constants,16);
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&viewport,
        .pRasterizationState=&raster,.pMultisampleState=&multi,.pDepthStencilState=draw->depth?&draw->depth_state:NULL,.pColorBlendState=&blend,.layout=w->layout,.renderPass=w->pass}; CHECK(w->CreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,NULL,&w->pipeline));
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=r->device.queue_family}; CHECK(r->CreateCommandPool(device,&pool,NULL,&r->pool));
    VkCommandBuffer command; VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=r->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; CHECK(r->AllocateCommandBuffers(device,&allocation,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CHECK(r->BeginCommandBuffer(command,&begin));
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT};
    r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,1,&host,0,NULL,0,NULL);
    VulkanTarget *images[18]; for(unsigned i=0;i<draw->texture_count;i++) images[i]=draw->textures[i].target; images[draw->texture_count]=draw->depth; images[draw->texture_count+1]=target;
    for(unsigned i=0;i<draw->texture_count+2;i++) {
        VulkanTarget *t=images[i]; if(!t) continue; bool seen=false; for(unsigned j=0;j<i;j++) if(images[j]==t) seen=true; if(seen) continue;
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask=t==target?VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:t==draw->depth?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_SHADER_READ_BIT,.oldLayout=t->layout,
            .newLayout=t==target?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:t==draw->depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->image,
            .subresourceRange={.aspectMask=t->depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
        r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,1,&barrier);
    }
    VkRenderPassBeginInfo pass_begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=w->pass,.framebuffer=w->framebuffer,.renderArea={.extent={target->width,target->height}}};
    r->CmdBeginRenderPass(command,&pass_begin,VK_SUBPASS_CONTENTS_INLINE); w->CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,w->pipeline);
    w->CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,w->layout,0,2,sets,0,NULL);
    VkDeviceSize offset=0; for(unsigned i=0;i<16;i++) if(vertices[i]) w->CmdBindVertexBuffers(command,i,1,&vertices[i],&offset);
    if(index_buffer) {w->CmdBindIndexBuffer(command,index_buffer,0,draw->index_type); w->CmdDrawIndexed(command,draw->count,draw->instances,0,draw->base_vertex,draw->base_instance);}
    else w->CmdDraw(command,draw->count,draw->instances,draw->first,draw->base_instance);
    r->CmdEndRenderPass(command); CHECK(r->EndCommandBuffer(command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CHECK(r->CreateFence(device,&fence,NULL,&r->fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command}; r->pending=true;
    CHECK(r->QueueSubmit(r->device.queue,1,&submit,r->fence)); CHECK(r->WaitForFences(device,1,&r->fence,VK_TRUE,5000000000ull)); r->pending=false;
    target->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for(unsigned i=0;i<draw->texture_count;i++) draw->textures[i].target->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(draw->depth) draw->depth->layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    work_destroy(r);
    return vulkan_render_pixels(r,target,false,pixels,capacity,error);
fail:
    if(!r->pending) work_destroy(r); return false;
}
#undef DRAW_FUNCTIONS
