#import <Foundation/Foundation.h>
#include "shader_prep.h"
#include "shader_ir_source.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static NSString *test_source(void) {
    return @"#include <metal_stdlib>\nusing namespace metal;\n"
        "template<typename T> [[clang::optnone]] T spvFAdd(T a,T b){return a+b;}\n"
        "template<typename T> [[clang::optnone]] T spvFMul(T a,T b){return a*b;}\n"
        "kernel void ir_check(device const float *v [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
        " float a=spvFAdd(v[i],0x1p-23f); float b=spvFMul(a,0x1.000002p0f);\n"
        " out[i*2]=as_type<uint>(spvFAdd(b,-v[i])); out[i*2+1]=as_type<uint>(fma(v[i],0x1.000002p0f,-v[i])); }\n";
}

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 3); NSString *path = @(argv[2]);
    if (!strcmp(argv[1], "emit")) return [test_source() writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil] ? 0 : 74;
    BOOL binary_pipeline = !strcmp(argv[1], "verify-compute");
    assert(binary_pipeline || !strcmp(argv[1], "verify"));
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 78;
    NSData *raw = [NSData dataWithContentsOfFile:path]; NSString *source = nil; MTLCompileOptions *options = nil;
    assert(shader_ir_source(raw, NO, &source, &options));
    id<MTLLibrary> offline = shader_ir_load(device, path, raw.bytes, raw.length, NO);
    id<MTLLibrary> online = [device newLibraryWithSource:source options:options error:nil];
    assert(offline && online);
    float inputs[64], expected[128];
    const float values[] = {0, 1, -1, 1e20f, -1e20f, 1e-20f, -1e-20f, 16777216.0f};
    for (unsigned i = 0; i < 64; i++) {
        inputs[i] = values[i % 8];
        volatile float a = inputs[i] + 0x1p-23f, b = a * 0x1.000002p0f;
        expected[i*2] = b - inputs[i]; expected[i*2+1] = fmaf(inputs[i], 0x1.000002p0f, -inputs[i]);
    }
    id<MTLBuffer> input = [device newBufferWithBytes:inputs length:sizeof inputs options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSData *baseline = nil;
    for (unsigned pass = 0; pass < (binary_pipeline ? 3u : 2u); pass++) {
        id<MTLFunction> function = [(pass ? offline : online) newFunctionWithName:@"ir_check"];
        id<MTLComputePipelineState> pipeline = pass == 2 ? shader_compute_load(device, path, function) :
            [device newComputePipelineStateWithFunction:function error:nil];
        assert(pipeline);
        id<MTLBuffer> output = [device newBufferWithLength:sizeof expected options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer]; id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setBuffer:input offset:0 atIndex:0]; [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(64,1,1) threadsPerThreadgroup:MTLSizeMake(8,1,1)]; [encoder endEncoding];
        [command commit]; [command waitUntilCompleted]; assert(command.status == MTLCommandBufferStatusCompleted);
        NSData *result = [NSData dataWithBytes:output.contents length:sizeof expected];
        assert(!memcmp(result.bytes, expected, sizeof expected));
        if (baseline) assert([baseline isEqual:result]); else baseline = result;
    }
    printf("{\"float_values\":128,\"bitwise_equal\":true,\"cpu_reference_equal\":true,\"binary_pipeline_verified\":%s}\n",binary_pipeline ? "true" : "false"); fflush(stdout);
    shader_ir_report("synthetic-gpu"); return 0;
} }
