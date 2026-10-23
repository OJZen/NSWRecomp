bool vulkan_render_pixels(VulkanRender *r,VulkanTarget *target,bool upload,void *pixels,size_t capacity,char error[256]) {
    size_t count=vulkan_render_target_pixels(r,target);
    if(!r || r->pending || !count || !pixels || (!upload && target->layout==VK_IMAGE_LAYOUT_UNDEFINED)) {snprintf(error,256,"invalid image transfer request"); return false;}
    size_t bytes=count*(target->depth?5:vulkan_color_bytes(target->format));
    if(capacity!=bytes || bytes>(64u<<20)) {snprintf(error,256,"invalid image transfer capacity"); return false;}
    VkDevice device=r->device.device; bool coherent=false;
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=upload?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(r->CreateBuffer(device,&buffer,NULL,&r->readback));
    VkMemoryRequirements requirements; r->GetBufferMemoryRequirements(device,r->readback,&requirements);
    if(!allocate(r,requirements,true,&r->readback_memory,&coherent,error)) goto fail;
    CHECK(r->BindBufferMemory(device,r->readback,r->readback_memory,0));
    if(upload) {
        void *mapped; CHECK(r->MapMemory(device,r->readback_memory,0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,pixels,bytes);
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=r->readback_memory,.size=VK_WHOLE_SIZE};
        VkResult flushed=coherent?VK_SUCCESS:r->FlushMappedMemoryRanges(device,1,&range); r->UnmapMemory(device,r->readback_memory); CHECK(flushed);
    }
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=r->device.queue_family};
    CHECK(r->CreateCommandPool(device,&pool,NULL,&r->pool));
    VkCommandBuffer command; VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=r->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(r->AllocateCommandBuffers(device,&allocation,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CHECK(r->BeginCommandBuffer(command,&begin));
    VkImageLayout layout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=target->layout?VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT:0,
        .dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=target->layout,.newLayout=layout,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=target->image,
        .subresourceRange={.aspectMask=target->depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,upload?1:0,upload?&host:NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copies[2]={
        {.imageSubresource={.aspectMask=target->depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={target->width,target->height,1}},
        {.bufferOffset=count*4,.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT,.layerCount=1},.imageExtent={target->width,target->height,1}}
    };
    if(upload) r->CmdCopyBufferToImage(command,r->readback,target->image,layout,target->depth?2:1,copies);
    else r->CmdCopyImageToBuffer(command,target->image,layout,r->readback,target->depth?2:1,copies);
    if(!upload) {
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    }
    CHECK(r->EndCommandBuffer(command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CHECK(r->CreateFence(device,&fence,NULL,&r->fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    r->pending=true; CHECK(r->QueueSubmit(r->device.queue,1,&submit,r->fence));
    CHECK(r->WaitForFences(device,1,&r->fence,VK_TRUE,5000000000ull)); r->pending=false; target->layout=layout;
    if(!upload) {
        void *mapped; CHECK(r->MapMemory(device,r->readback_memory,0,VK_WHOLE_SIZE,0,&mapped));
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=r->readback_memory,.size=VK_WHOLE_SIZE};
        VkResult invalidated=coherent?VK_SUCCESS:r->InvalidateMappedMemoryRanges(device,1,&range);
        if(invalidated==VK_SUCCESS) memcpy(pixels,mapped,bytes); r->UnmapMemory(device,r->readback_memory); CHECK(invalidated);
    }
    work_destroy(r); return true;
fail:
    if(!r->pending) work_destroy(r); return false;
}
static bool depth_resize(VulkanRender *r,VulkanTarget *source,VulkanTarget *destination,void *pixels,size_t capacity,char error[256]) {
    size_t from_count=(size_t)source->width*source->height,to_count=(size_t)destination->width*destination->height;
    uint8_t *from=malloc(from_count*5),*expected=malloc(to_count*5);
    if(!from || !expected) {free(from); free(expected); snprintf(error,256,"depth resize allocation failed"); return false;}
    bool ok=vulkan_render_pixels(r,source,false,from,from_count*5,error);
    if(ok) {
        for(uint32_t y=0;y<destination->height;y++) for(uint32_t x=0;x<destination->width;x++) {
            size_t px=((uint64_t)x*2+1)*source->width/(destination->width*2),py=((uint64_t)y*2+1)*source->height/(destination->height*2);
            size_t i=(size_t)y*destination->width+x,j=py*source->width+px;
            memcpy(expected+i*4,from+j*4,4); expected[to_count*4+i]=from[from_count*4+j];
        }
        ok=vulkan_render_pixels(r,destination,true,expected,to_count*5,error) && vulkan_render_pixels(r,destination,false,pixels,capacity,error);
        if(ok) for(size_t i=0;i<to_count;i++) if(memcmp(expected+i*4,(uint8_t *)pixels+i*4,3) || expected[to_count*4+i]!=((uint8_t *)pixels)[to_count*4+i]) {
            snprintf(error,256,"depth resize round-trip mismatch"); ok=false; break;
        }
    }
    free(from); free(expected); return ok;
}
