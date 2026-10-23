#include "vulkan_compute.h"
#include "vulkan_windows.h"
#include "vulkan_compute_spirv.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    char error[256]; VulkanLoader loader; VulkanDevice device;
    if(!vulkan_windows_load(&loader,error)) {fprintf(stderr,"%s\n",error); return 78;}
    VulkanHost *host=vulkan_host_create(loader.get_instance_proc,-1,error);
    if(!host || !vulkan_host_device(host,&device)) {fprintf(stderr,"%s\n",error); return 78;}
    VulkanCompute *compute=vulkan_compute_create(&device,error);
    if(!compute) {fprintf(stderr,"%s\n",error); return 78;}
    uint32_t values[32],expected[32],groups[3]={4,1,1};
    for(unsigned i=0;i<32;i++) values[i]=expected[i]=0x12340000+i;
    VulkanComputeBuffer binding={.binding=0,.storage=true,.size=sizeof values,.input=values,.output=values};
    for(unsigned pass=0;pass<3;pass++) {
        if(!vulkan_compute_dispatch(compute,synthetic_compute_spirv,sizeof synthetic_compute_spirv,groups,&binding,1,error)) {fprintf(stderr,"%s\n",error); return 78;}
        for(unsigned i=0;i<32;i++) {expected[i]=expected[i]*3+7; if(values[i]!=expected[i]) {fprintf(stderr,"compute mismatch at %u\n",i); return 78;}}
    }
    if(!vulkan_compute_close(compute) || !vulkan_host_close(host)) return 78;
    vulkan_windows_unload(&loader);
    puts("{\"synthetic_compute_verified\":true,\"dispatches\":3,\"verified_words\":96,\"game_executed\":false}");
    return 0;
}
