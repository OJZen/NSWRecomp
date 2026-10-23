#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef enum {
    VULKAN_COLOR_RGBA8,
    VULKAN_COLOR_SRGBA8,
    VULKAN_COLOR_RGB11F,
    VULKAN_COLOR_R8,
    VULKAN_COLOR_RGBA16F,
    VULKAN_COLOR_COUNT
} VulkanColorFormat;
static inline unsigned vulkan_color_bytes(VulkanColorFormat format) {return format==VULKAN_COLOR_R8?1:format==VULKAN_COLOR_RGBA16F?8:4;}
static inline float vulkan_half_float(uint16_t h) {
    unsigned exponent=(h>>10)&31,mantissa=h&1023;
    float value=exponent==31?(mantissa?NAN:INFINITY):ldexpf((float)(mantissa+(exponent?1024:0)),(exponent?(int)exponent-15:-14)-10);
    return h&0x8000?-value:value;
}
static inline void vulkan_rgb11_float(const void *pixel,float rgba[4]) {
    uint32_t packed; memcpy(&packed,pixel,4);
    for(unsigned i=0;i<3;i++) {
        unsigned bits=i==2?5:6,field=(packed>>(i==0?0:i==1?11:22))&((1u<<(bits+5))-1),exponent=field>>bits,mantissa=field&((1u<<bits)-1);
        rgba[i]=exponent==31?(mantissa?NAN:INFINITY):ldexpf((float)(mantissa+(exponent?(1u<<bits):0)),(exponent?(int)exponent-15:-14)-(int)bits);
    }
    rgba[3]=1;
}
static inline void vulkan_color_decode(VulkanColorFormat format,const void *data,float rgba[4]) {
    const uint8_t *pixel=data;
    if(format==VULKAN_COLOR_RGB11F) {vulkan_rgb11_float(pixel,rgba); return;}
    if(format==VULKAN_COLOR_RGBA16F) {for(unsigned i=0;i<4;i++) {uint16_t h; memcpy(&h,pixel+i*2,2); rgba[i]=vulkan_half_float(h);} return;}
    rgba[0]=pixel[0]/255.f; rgba[1]=rgba[2]=0; rgba[3]=1;
    if(format!=VULKAN_COLOR_R8) for(unsigned i=1;i<4;i++) rgba[i]=pixel[i]/255.f;
    if(format==VULKAN_COLOR_SRGBA8) for(unsigned i=0;i<3;i++) rgba[i]=rgba[i]<=0.04045f?rgba[i]/12.92f:powf((rgba[i]+0.055f)/1.055f,2.4f);
}

// Verify the stored attachment representation, including its format precision.
static inline bool vulkan_color_matches(VulkanColorFormat format,const unsigned char pixel[4],const float color[4]) {
    if(format==VULKAN_COLOR_RGBA16F) {
        for(unsigned i=0;i<4;i++) {uint16_t h; memcpy(&h,pixel+i*2,2); float decoded=vulkan_half_float(h);
            if(!isfinite(decoded) || fabsf(decoded-color[i])>fmaxf(0.000001f,fabsf(color[i])/1024)) return false;}
        return true;
    }
    if(format==VULKAN_COLOR_RGB11F) {
        uint32_t packed; memcpy(&packed,pixel,4);
        for(unsigned i=0;i<3;i++) {
            unsigned bits=i==2?5:6, field=(packed>>(i==0?0:i==1?11:22))&((1u<<(bits+5))-1);
            unsigned exponent=field>>bits, mantissa=field&((1u<<bits)-1);
            if(exponent==31) return false;
            float step=ldexpf(1.f,(exponent?(int)exponent-15:-14)-(int)bits);
            float decoded=step*(mantissa+(exponent?(1u<<bits):0));
            if(fabsf(decoded-color[i])>step) return false;
        }
        return true;
    }
    for(unsigned i=0;i<(format==VULKAN_COLOR_R8?1u:4u);i++) {
        float value=color[i];
        if(format==VULKAN_COLOR_SRGBA8 && i<3) value=value<=0.0031308f?12.92f*value:1.055f*powf(value,1.f/2.4f)-0.055f;
        if(fabsf((float)pixel[i]-roundf(value*255))>1) return false;
    }
    return true;
}
