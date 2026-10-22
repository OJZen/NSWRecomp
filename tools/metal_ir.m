#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "shader_prep.h"
#include "shader_ir_source.h"
#include <string.h>
#include <time.h>

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3 || (strcmp(argv[1], "prepare") && strcmp(argv[1], "verify") &&
                      strcmp(argv[1], "compute-plan") && strcmp(argv[1], "compute-verify"))) return 64;
    BOOL verify = !strcmp(argv[1], "verify");
    BOOL compute = !strcmp(argv[1], "compute-plan") || !strcmp(argv[1], "compute-verify");
    BOOL verify_compute = !strcmp(argv[1], "compute-verify");
    NSData *plan_data = [NSData dataWithContentsOfFile:@(argv[2])];
    NSArray *plan = plan_data ? [NSJSONSerialization JSONObjectWithData:plan_data options:0 error:nil] : nil;
    if (![plan isKindOfClass:NSArray.class] || !plan.count || plan.count > 256) return 65;
    id<MTLDevice> device = verify || compute ? MTLCreateSystemDefaultDevice() : nil;
    if ((verify || compute) && !device) { fprintf(stderr, "Metal device unavailable\n"); return 78; }
    NSMutableArray *results = [NSMutableArray array];
    for (NSDictionary *job in plan) { @autoreleasepool {
        if (![job isKindOfClass:NSDictionary.class] || ![job[@"source"] isKindOfClass:NSString.class]) return 65;
        NSData *raw = [NSData dataWithContentsOfFile:job[@"source"]]; NSString *source = nil; MTLCompileOptions *options = nil;
        NSDictionary *description = shader_ir_source(raw, [job[@"layered"] boolValue], &source, &options);
        if (!description) { fprintf(stderr, "Unsupported shader source transform\n"); return 78; }
        NSMutableDictionary *result = [description mutableCopy];
        if (compute) {
            if (plan.count > 32 || ![job[@"function"] isKindOfClass:NSString.class]) return 65;
            id<MTLLibrary> library = shader_ir_load(device, job[@"source"], raw.bytes, raw.length, NO);
            id<MTLFunction> function = [library newFunctionWithName:job[@"function"]];
            if (!function || function.functionType != MTLFunctionTypeKernel) return 78;
            result = [shader_compute_identity(device, description, function.name) mutableCopy];
            if (!result) return 78;
            if (verify_compute) {
                uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                id<MTLComputePipelineState> pipeline = shader_compute_load(device, job[@"source"], function);
                if (!pipeline) { fprintf(stderr, "Strict compute binary miss: %s\n", [job[@"source"] UTF8String]); return 78; }
                result[@"pipeline_ms"] = @((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6);
                result[@"strict_archive_hit"] = @YES;
                id<MTLComputePipelineState> reference = [device newComputePipelineStateWithFunction:function error:nil];
                if (!reference || pipeline.threadExecutionWidth != reference.threadExecutionWidth ||
                    pipeline.maxTotalThreadsPerThreadgroup != reference.maxTotalThreadsPerThreadgroup ||
                    pipeline.staticThreadgroupMemoryLength != reference.staticThreadgroupMemoryLength) return 78;
                result[@"thread_limits_match"] = @YES;
            }
            [results addObject:result]; continue;
        }
        if (!verify) {
            if (![job[@"prepared"] isKindOfClass:NSString.class] || ![source writeToFile:job[@"prepared"] atomically:YES encoding:NSUTF8StringEncoding error:nil]) return 74;
        } else {
            uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            id<MTLLibrary> library = shader_ir_load(device, job[@"source"], raw.bytes, raw.length, [job[@"layered"] boolValue]);
            result[@"load_ms"] = @((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6);
            if (!library) { fprintf(stderr, "Offline library rejected\n"); return 78; }
            NSError *error = nil;
            start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            id<MTLLibrary> reference = [device newLibraryWithSource:source options:options error:&error];
            result[@"source_compile_ms"] = @((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6);
            if (!reference || ![[NSSet setWithArray:library.functionNames] isEqual:[NSSet setWithArray:reference.functionNames]]) return 78;
            NSString *reflection_path = [[job[@"source"] stringByDeletingPathExtension] stringByAppendingPathExtension:@"json"];
            NSData *reflection_data = [NSData dataWithContentsOfFile:reflection_path];
            NSDictionary *reflection = reflection_data ? [NSJSONSerialization JSONObjectWithData:reflection_data options:0 error:nil] : nil;
            if (reflection && ![reflection isKindOfClass:NSDictionary.class]) return 65;
            if ([reflection[@"route"] isEqual:@"B"] && [reflection[@"bindings"] count]) {
                NSString *entry = reflection[@"entry"];
                id<MTLFunction> function = [library newFunctionWithName:entry], original = [reference newFunctionWithName:entry];
                if (!function || !original) return 78;
                NSUInteger bytes = [function newArgumentEncoderWithBufferIndex:0].encodedLength;
                if (!bytes || bytes != [original newArgumentEncoderWithBufferIndex:0].encodedLength) return 78;
                result[@"argument_bytes"] = @(bytes);
            }
            unsigned computes = 0;
            for (NSString *name in library.functionNames) {
                id<MTLFunction> function = [library newFunctionWithName:name], original = [reference newFunctionWithName:name];
                if (!function || function.functionType != original.functionType) return 78;
                if (function.functionType == MTLFunctionTypeKernel) {
                    if (![device newComputePipelineStateWithFunction:function error:&error]) { fprintf(stderr, "Compute PSO: %s\n", error.localizedDescription.UTF8String); return 78; }
                    computes++;
                }
            }
            result[@"functions"] = library.functionNames; result[@"compute_pipelines"] = @(computes);
        }
        [results addObject:result];
    } }
    NSData *output = [NSJSONSerialization dataWithJSONObject:results options:NSJSONWritingSortedKeys error:nil];
    if (!output) return 65;
    fwrite(output.bytes, 1, output.length, stdout); fputc('\n', stdout); fflush(stdout);
    if (verify || verify_compute) shader_ir_report("probe");
    return 0;
} }
