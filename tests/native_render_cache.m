#import <Foundation/Foundation.h>
#include "shader_prep.h"
#include "shader_ir_source.h"
#include <assert.h>

@interface RenderDevice : NSObject
@property unsigned archives, pipelines;
@property BOOL miss;
@end
@implementation RenderDevice
- (id)architecture { return self; }
- (NSString *)name { return @"synthetic_arch"; }
- (uint64_t)registryID { return 1; }
- (id)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    (void)error; assert(descriptor.url.isFileURL); self.archives++; return [NSObject new];
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    reflection:(MTLRenderPipelineReflection **)reflection error:(NSError **)error {
    (void)reflection; (void)error;
    assert(descriptor.binaryArchives.count == 1 && options == MTLPipelineOptionFailOnBinaryArchiveMiss);
    self.pipelines++; return self.miss ? nil : [NSObject new];
}
@end

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 3); NSString *directory = @(argv[1]), *mode = @(argv[2]);
    RenderDevice *device = [RenderDevice new]; device.miss = [mode isEqual:@"driver-miss"];
    NSString *key = [@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0];
    NSString *cache = [directory stringByAppendingPathComponent:@"render-v1"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:cache withIntermediateDirectories:NO attributes:nil error:nil]);
    NSData *binary = [@"synthetic binary" dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableDictionary *manifest = [shader_render_context((id)device, directory, @"contract") mutableCopy];
    manifest[@"keys"] = @[key]; manifest[@"binary_sha256"] = shader_ir_sha(binary);
    if ([mode isEqual:@"stale"]) manifest[@"contract"] = @"stale";
    if ([mode isEqual:@"duplicate"]) manifest[@"keys"] = @[key,key];
    NSData *record = [NSJSONSerialization dataWithJSONObject:manifest options:0 error:nil];
    assert([record writeToFile:[cache stringByAppendingPathComponent:@"manifest.json"] atomically:YES]);
    NSString *path = [cache stringByAppendingPathComponent:@"pipelines.metallib"];
    if ([mode isEqual:@"symlink"]) {
        NSString *outside = [directory stringByAppendingPathComponent:@"preserved"];
        assert([binary writeToFile:outside atomically:YES]);
        assert([NSFileManager.defaultManager createSymbolicLinkAtPath:path withDestinationPath:outside error:nil]);
    } else assert([([mode isEqual:@"corrupt"] ? [NSData dataWithBytes:"bad" length:3] : binary) writeToFile:path atomically:YES]);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id pipeline = shader_render_load((id)device, d, key, directory, @"contract");
    if ([mode isEqual:@"valid"]) {
        assert(pipeline && device.archives == 1 && device.pipelines == 1 && !d.binaryArchives.count);
        assert(!shader_render_load((id)device, d, @"unknown", directory, @"contract") && device.pipelines == 1);
        assert(!shader_render_load((id)device, d, key, directory, @"changed") && device.pipelines == 1);
    } else {
        assert(!pipeline);
        assert(device.pipelines == ([mode isEqual:@"driver-miss"] ? 1 : 0));
    }
    NSError *error = nil;
    assert(!shader_render_capture((id)device, @[d,d], @[key,key], directory, @"contract", [directory stringByAppendingPathComponent:@"capture"], &error));
    assert(error);
    shader_render_report(); return 0;
} }
