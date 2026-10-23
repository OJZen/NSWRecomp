#pragma once
#include "vulkan_host.h"
#include <stdio.h>

// Direct input locations emitted by the pinned translator. Unused NVN vertex
// attributes can remain bound across draws and must not trigger buffer fetches.
static inline bool vulkan_vertex_locations(const uint32_t *words,size_t bytes,uint32_t *mask,char error[256]) {
    if(!words || bytes<20 || bytes>(4u<<20) || bytes%4 || words[0]!=0x07230203) {snprintf(error,256,"invalid vertex SPIR-V"); return false;}
    uint32_t inputs[64]; unsigned count=0; *mask=0;
    for(size_t i=5;i<bytes/4;) {
        unsigned size=words[i]>>16,op=words[i]&65535;
        if(!size || size>bytes/4-i) {snprintf(error,256,"truncated vertex SPIR-V"); return false;}
        if(op==59 && size>=4 && words[i+3]==1) {
            if(count==64) {snprintf(error,256,"vertex input limit"); return false;} inputs[count++]=words[i+2];
        }
        i+=size;
    }
    for(size_t i=5;i<bytes/4;i+=words[i]>>16) {
        unsigned size=words[i]>>16,op=words[i]&65535;
        if(op!=71 || size<4 || words[i+2]!=30) continue;
        for(unsigned j=0;j<count;j++) if(inputs[j]==words[i+1]) {
            if(size!=4 || words[i+3]>=16 || (*mask&(1u<<words[i+3]))) {snprintf(error,256,"unsupported vertex input location"); return false;}
            *mask|=1u<<words[i+3];
        }
    }
    return true;
}

// Structural bounds and the enabled feature contract, not a SPIR-V validator.
// Translation remains tied to reviewed local sources and an exact source packet.
static inline bool vulkan_shader_supported(const VulkanDevice *device,const uint32_t *words,size_t bytes,unsigned model,char error[256]) {
    if(!words || bytes<20 || bytes>(4u<<20) || bytes%4 || words[0]!=0x07230203 || words[4]) {snprintf(error,256,"invalid SPIR-V header"); return false;}
    bool entry=false;
    for(size_t i=5;i<bytes/4;) {
        unsigned size=words[i]>>16,op=words[i]&65535;
        if(!size || size>bytes/4-i) {snprintf(error,256,"truncated SPIR-V instruction"); return false;}
        if(op==17) {
            if(size!=2) {snprintf(error,256,"invalid SPIR-V capability"); return false;}
            switch(words[i+1]) {
                case 1: case 46: case 47: case 50: case 62: case 64: case 65: break;
                case 25: if(device->image_gather_extended) break; snprintf(error,256,"SPIR-V requires imageGatherExtended"); return false;
                case 4427: if(device->draw_parameters) break; snprintf(error,256,"SPIR-V requires shaderDrawParameters"); return false;
                default: snprintf(error,256,"unsupported SPIR-V capability %u",words[i+1]); return false;
            }
        }
        if(op==15) {
            if(size<5 || words[i+1]!=model || words[i+3]!=0x6e69616d || words[i+4]) {snprintf(error,256,"unsupported SPIR-V entry point"); return false;}
            entry=true;
        }
        i+=size;
    }
    if(!entry) snprintf(error,256,"missing SPIR-V entry point"); return entry;
}
