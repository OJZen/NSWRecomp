// Full-subresource copies retain the source and compare both complete readbacks.
bool vulkan_render_copy(VulkanRender *r,VulkanTarget *source,VulkanTarget *destination,bool linear,void *pixels,size_t capacity,char error[256]) {
    if(!r || r->pending || !source || !destination || source==destination || !vulkan_render_target_pixels(r,source) || !vulkan_render_target_pixels(r,destination)) {
        snprintf(error,256,"invalid image copy ownership or pending work"); return false;
    }
    size_t count=(size_t)source->width*source->height;
    unsigned bpp=source->depth?5:vulkan_color_bytes(source->format); size_t bytes=count*bpp;
    size_t destination_count=(size_t)destination->width*destination->height;
    size_t destination_bytes=destination_count*(destination->depth?5:vulkan_color_bytes(destination->format));
    bool convert=!source->depth && !destination->depth && source->format!=destination->format;
    bool resize=source->width!=destination->width || source->height!=destination->height,blit=convert || resize || linear;
    if((source->depth && linear) || (source->format!=destination->format && !convert) || source->depth!=destination->depth ||
        source->layout==VK_IMAGE_LAYOUT_UNDEFINED || bytes>(64u<<20) || destination_bytes>(64u<<20) || !pixels || capacity!=destination_bytes) {
        snprintf(error,256,"unsupported image copy format, extent, source contents or readback capacity"); return false;
    }
    if(blit) {
        VkFormatProperties src,dst;
        if(!r->device.format_properties || !r->device.physical) {snprintf(error,256,"missing Vulkan format capability query"); return false;}
        r->device.format_properties(r->device.physical,source->depth?VK_FORMAT_D24_UNORM_S8_UINT:formats[source->format],&src);
        r->device.format_properties(r->device.physical,destination->depth?VK_FORMAT_D24_UNORM_S8_UINT:formats[destination->format],&dst);
        if(!(src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dst.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT) || (linear && !(src.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))) {
            if(source->depth && resize && !linear) return depth_resize(r,source,destination,pixels,capacity,error);
            snprintf(error,256,"unsupported Vulkan format blit"); return false;
        }
    }
    VkDevice device=r->device.device; bool coherent=false; VkDeviceSize stride=(bytes+3)&~(VkDeviceSize)3;
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=stride+destination_bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(r->CreateBuffer(device,&buffer,NULL,&r->readback));
    VkMemoryRequirements requirements; r->GetBufferMemoryRequirements(device,r->readback,&requirements);
    if(!allocate(r,requirements,true,&r->readback_memory,&coherent,error)) goto fail;
    CHECK(r->BindBufferMemory(device,r->readback,r->readback_memory,0));
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=r->device.queue_family};
    CHECK(r->CreateCommandPool(device,&pool,NULL,&r->pool));
    VkCommandBuffer command; VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=r->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(r->AllocateCommandBuffers(device,&allocation,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CHECK(r->BeginCommandBuffer(command,&begin));
    VkImageAspectFlags aspects=source->depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier barriers[2]={
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout=source->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
         .image=source->image,.subresourceRange={.aspectMask=aspects,.levelCount=1,.layerCount=1}},
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=destination->layout?VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT:0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout=destination->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
         .image=destination->image,.subresourceRange={.aspectMask=aspects,.levelCount=1,.layerCount=1}}
    };
    r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,barriers);
    VkImageCopy copies[2]={
        {.srcSubresource={.aspectMask=source->depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},
         .dstSubresource={.aspectMask=source->depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.extent={source->width,source->height,1}},
        {.srcSubresource={.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT,.layerCount=1},.dstSubresource={.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT,.layerCount=1},.extent={source->width,source->height,1}}
    };
    if(blit) {
        VkImageBlit regions[2]={{.srcSubresource=copies[0].srcSubresource,.dstSubresource=copies[0].dstSubresource,
            .srcOffsets={{0,0,0},{(int)source->width,(int)source->height,1}},.dstOffsets={{0,0,0},{(int)destination->width,(int)destination->height,1}}}};
        regions[1]=regions[0]; regions[1].srcSubresource=copies[1].srcSubresource; regions[1].dstSubresource=copies[1].dstSubresource;
        r->CmdBlitImage(command,source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,source->depth?2:1,regions,linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    } else r->CmdCopyImage(command,source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,source->depth?2:1,copies);
    barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barriers[1]);
    // Buffer offsets must be four-byte aligned, including odd-sized R8 images.
    for(unsigned i=0;i<2;i++) {
        VkBufferImageCopy copy={.bufferOffset=i*stride,.imageSubresource={.aspectMask=source->depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={i?destination->width:source->width,i?destination->height:source->height,1}};
        r->CmdCopyImageToBuffer(command,i?destination->image:source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->readback,1,&copy);
        if(source->depth) {copy.bufferOffset+=(i?destination_count:count)*4; copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
            r->CmdCopyImageToBuffer(command,i?destination->image:source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->readback,1,&copy);}
    }
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    r->CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(r->EndCommandBuffer(command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CHECK(r->CreateFence(device,&fence,NULL,&r->fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    r->pending=true; CHECK(r->QueueSubmit(r->device.queue,1,&submit,r->fence));
    CHECK(r->WaitForFences(device,1,&r->fence,VK_TRUE,5000000000ull)); r->pending=false;
    source->layout=destination->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    void *mapped; CHECK(r->MapMemory(device,r->readback_memory,0,VK_WHOLE_SIZE,0,&mapped));
    if(!coherent) {VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=r->readback_memory,.size=VK_WHOLE_SIZE};
        VkResult result=r->InvalidateMappedMemoryRanges(device,1,&range); if(result!=VK_SUCCESS) {r->UnmapMemory(device,r->readback_memory); failure(error,"copy invalidate",result); goto fail;}}
    bool equal=true; const uint8_t *a=mapped,*b=a+stride;
    if(source->depth) {
        for(uint32_t y=0;y<destination->height && equal;y++) for(uint32_t x=0;x<destination->width;x++) {
            size_t px=((uint64_t)x*2+1)*source->width/(destination->width*2),py=((uint64_t)y*2+1)*source->height/(destination->height*2);
            size_t from=py*source->width+px,to=(size_t)y*destination->width+x;
            if(memcmp(a+from*4,b+to*4,3) || a[count*4+from]!=b[destination_count*4+to]) {equal=false; break;}
        }
    } else if(blit) {
        unsigned destination_bpp=vulkan_color_bytes(destination->format);
        for(uint32_t y=0;y<destination->height && equal;y++) for(uint32_t x=0;x<destination->width;x++) {
            float sx=((float)x+0.5f)*source->width/destination->width-0.5f,sy=((float)y+0.5f)*source->height/destination->height-0.5f;
            int x0=linear?(int)floorf(sx):(int)floorf(sx+0.5f),y0=linear?(int)floorf(sy):(int)floorf(sy+0.5f);
            float fx=linear?sx-floorf(sx):0,fy=linear?sy-floorf(sy):0,color[4]={0};
            for(unsigned k=0;k<4;k++) {
                int px=x0+(int)(k&1),py=y0+(int)(k>>1); if(px<0) px=0; if(py<0) py=0; if(px>=(int)source->width) px=source->width-1; if(py>=(int)source->height) py=source->height-1;
                float c[4],weight=(k&1?fx:1-fx)*(k&2?fy:1-fy); vulkan_color_decode(source->format,a+((size_t)py*source->width+px)*bpp,c);
                for(unsigned j=0;j<4;j++) color[j]+=c[j]*weight;
            }
            if(destination->format!=VULKAN_COLOR_RGB11F && destination->format!=VULKAN_COLOR_RGBA16F) for(unsigned j=0;j<4;j++) color[j]=fminf(1,fmaxf(0,color[j]));
            if(!vulkan_color_matches(destination->format,b+((size_t)y*destination->width+x)*destination_bpp,color)) {equal=false; break;}
        }
    } else equal=!memcmp(a,b,bytes);
    if(equal) memcpy(pixels,b,destination_bytes);
    r->UnmapMemory(device,r->readback_memory);
    if(!equal) {snprintf(error,256,"Vulkan image copy readback mismatch"); goto fail;}
    work_destroy(r); return true;
fail:
    if(!r->pending) work_destroy(r); return false;
}
