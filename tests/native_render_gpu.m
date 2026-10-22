#import <Foundation/Foundation.h>
#include "shader_prep.h"
#include <assert.h>
#include <string.h>

static NSData *draw(id<MTLDevice> device, id<MTLRenderPipelineState> pipeline) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td]; assert(texture);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
    id<MTLCommandQueue> queue = [device newCommandQueue]; id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted]; assert(command.status == MTLCommandBufferStatusCompleted);
    NSMutableData *data = [NSMutableData dataWithLength:16*16*4];
    [texture getBytes:data.mutableBytes bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
    return data;
}

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 3); BOOL capture = !strcmp(argv[1], "capture"), emit = !strcmp(argv[1], "emit");
    assert(capture || emit || !strcmp(argv[1], "verify"));
    NSString *root = @(argv[2]), *directory = [root stringByAppendingPathComponent:@"shaders/b-synthetic"];
    NSString *source_path = [directory stringByAppendingPathComponent:@"synthetic_b.metallib"];
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 p [[position]]; };\n"
        "vertex O v(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }\n"
        "fragment float4 f(){return float4(0.25,0.5,0.75,1);}\nfragment float4 other(){return float4(1,0,0,1);}\n";
    if (emit) {
        assert([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]);
        return [source writeToFile:source_path atomically:YES encoding:NSUTF8StringEncoding error:nil] ? 0 : 74;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 78;
    NSError *error = nil; NSData *raw = [NSData dataWithContentsOfFile:source_path];
    id<MTLLibrary> library = shader_ir_load(device, source_path, raw.bytes, raw.length, NO); assert(library);
    NSMutableArray *descriptors = [NSMutableArray array];
    NSArray *keys = @[[ @"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0],
                      [ @"b" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0]];
    for (unsigned i = 0; i < 2; i++) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:@"v"]; d.fragmentFunction = [library newFunctionWithName:@"f"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (i) d.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskAlpha;
        [descriptors addObject:d];
    }
    if (capture) {
        if (!shader_render_capture(device, descriptors, keys, directory, @"synthetic-render-v1", [root stringByAppendingPathComponent:@"capture"], &error)) {
            fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 78;
        }
        puts("{\"captured\":2}"); return 0;
    }
    for (unsigned i = 0; i < 2; i++) {
        MTLRenderPipelineDescriptor *d = descriptors[i];
        id<MTLRenderPipelineState> prepared = shader_render_load(device, d, keys[i], directory, @"synthetic-render-v1"); assert(prepared);
        assert(!d.binaryArchives.count);
        id<MTLRenderPipelineState> reference = [device newRenderPipelineStateWithDescriptor:d error:&error]; assert(reference);
        NSData *actual = draw(device, prepared); assert([actual isEqual:draw(device, reference)]);
        const uint8_t *bytes = actual.bytes;
        for (unsigned p = 0; p < 256; p++) assert(bytes[p*4]==64 && bytes[p*4+1]==(i?0:128) && bytes[p*4+2]==(i?0:191) && bytes[p*4+3]==255);
    }
    MTLRenderPipelineDescriptor *missing = [descriptors[0] copy]; missing.fragmentFunction = [library newFunctionWithName:@"other"];
    assert(!shader_render_load(device, missing, keys[0], directory, @"synthetic-render-v1"));
    puts("{\"render_pipelines\":2,\"pixels_compared\":512,\"byte_identical\":true,\"cpu_reference_equal\":true,\"uncaptured_function_rejected\":true}");
    shader_render_report(); return 0;
} }
