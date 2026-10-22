#import <Foundation/Foundation.h>
#include "shader_prep.h"
#include "shader_ir_source.h"
#include <assert.h>

@interface TestIRDevice : NSObject
@property unsigned loads;
@property BOOL binary_miss;
- (id)architecture;
- (NSString *)name;
- (uint64_t)registryID;
- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
- (id)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error;
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    reflection:(MTLComputePipelineReflection **)reflection error:(NSError **)error;
@end
@implementation TestIRDevice
- (id)architecture { return self; }
- (NSString *)name { return @"synthetic_arch"; }
- (uint64_t)registryID { return 1; }
- (id)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    (void)error; assert(descriptor.url.isFileURL); return [NSObject new];
}
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    reflection:(MTLComputePipelineReflection **)reflection error:(NSError **)error {
    (void)reflection; (void)error;
    assert(descriptor.computeFunction && descriptor.binaryArchives.count == 1 && options == MTLPipelineOptionFailOnBinaryArchiveMiss);
    return self.binary_miss ? nil : [NSObject new];
}
- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    (void)error; assert(dispatch_data_get_size(data) == 8); self.loads++;
    return [NSObject new];
}
@end

@interface TestIRFunction : NSObject
- (NSString *)name;
- (MTLFunctionType)functionType;
@end
@implementation TestIRFunction
- (NSString *)name { return @"synthetic_compute"; }
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }
@end

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 2);
    NSString *directory = @(argv[1]), *path = [directory stringByAppendingPathComponent:@"test.metallib"];
    NSString *source = @"template<typename T>\n[[clang::optnone]] T spvFAdd(T a, T b) { return a + b; }\n";
    NSData *input = [source dataUsingEncoding:NSUTF8StringEncoding];
    NSString *prepared = nil; MTLCompileOptions *options = nil;
    NSDictionary *identity = shader_ir_source(input, NO, &prepared, &options);
    assert(identity && options.mathMode == MTLMathModeSafe && options.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsPrecise);
    assert([prepared hasPrefix:@"#pragma clang fp contract(off)"]);
    assert([prepared rangeOfString:@"return a + b;"].location != NSNotFound);
    assert(!shader_ir_source(input, YES, NULL, NULL));
    assert(!shader_ir_source([NSData dataWithBytes:"\xff" length:1], NO, NULL, NULL));
    TestIRDevice *device = [TestIRDevice new];
    assert(!shader_ir_load((id)device, path, input.bytes, input.length, NO) && !device.loads);
    NSString *cache = [directory stringByAppendingPathComponent:@"ir-v1"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:cache withIntermediateDirectories:NO attributes:nil error:nil]);
    NSString *base = [cache stringByAppendingPathComponent:identity[@"key"]];
    NSString *binary_path = [base stringByAppendingString:@".metallib"], *metadata_path = [base stringByAppendingString:@".json"];
    NSData *binary = [NSData dataWithBytes:"MTLBtest" length:8];
    NSMutableDictionary *metadata = [identity mutableCopy]; metadata[@"library_sha256"] = shader_ir_sha(binary);
    NSData *(^record)(void) = ^{ return [NSJSONSerialization dataWithJSONObject:metadata options:0 error:nil]; };
    NSError *error = nil;
    BOOL wrote_binary = [binary writeToFile:binary_path options:NSDataWritingAtomic error:&error];
    if (!wrote_binary) fprintf(stderr, "Synthetic binary write: %s\n", error.localizedDescription.UTF8String);
    BOOL wrote_record = [record() writeToFile:metadata_path options:NSDataWritingAtomic error:&error];
    if (!wrote_record) fprintf(stderr, "Synthetic metadata write: %s\n", error.localizedDescription.UTF8String);
    assert(wrote_binary && wrote_record);
    assert(shader_ir_load((id)device, path, input.bytes, input.length, NO) && device.loads == 1);
    NSData *changed = [[source stringByAppendingString:@"// changed\n"] dataUsingEncoding:NSUTF8StringEncoding];
    assert(!shader_ir_load((id)device, path, changed.bytes, changed.length, NO) && device.loads == 1);
    metadata[@"os"] = @"stale"; assert([record() writeToFile:metadata_path atomically:YES]);
    assert(!shader_ir_load((id)device, path, input.bytes, input.length, NO) && device.loads == 1);
    metadata[@"os"] = identity[@"os"]; assert([record() writeToFile:metadata_path atomically:YES]);
    assert([[NSData dataWithBytes:"MTLBbad!" length:8] writeToFile:binary_path atomically:YES]);
    assert(!shader_ir_load((id)device, path, input.bytes, input.length, NO) && device.loads == 1);
    assert([NSFileManager.defaultManager removeItemAtPath:binary_path error:nil]);
    NSString *outside = [directory stringByAppendingPathComponent:@"preserved"];
    assert([binary writeToFile:outside atomically:YES]);
    assert([NSFileManager.defaultManager createSymbolicLinkAtPath:binary_path withDestinationPath:outside error:nil]);
    assert(!shader_ir_load((id)device, path, input.bytes, input.length, NO) && device.loads == 1);
    assert([[NSData dataWithContentsOfFile:outside] isEqual:binary]);
    assert([[source dataUsingEncoding:NSUTF8StringEncoding] isEqual:input]);
    assert([input writeToFile:path atomically:YES]);
    id<MTLFunction> function = (id)[TestIRFunction new];
    assert(!shader_compute_load((id)device, path, function));
    NSDictionary *compute_identity = shader_compute_identity((id)device, identity, function.name);
    NSString *compute_directory = [directory stringByAppendingPathComponent:@"compute-v1"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:compute_directory withIntermediateDirectories:NO attributes:nil error:nil]);
    NSString *compute_base = [compute_directory stringByAppendingPathComponent:compute_identity[@"key"]];
    NSString *compute_binary = [compute_base stringByAppendingString:@".metallib"], *compute_record = [compute_base stringByAppendingString:@".json"];
    NSMutableDictionary *compute_metadata = [compute_identity mutableCopy]; compute_metadata[@"binary_sha256"] = shader_ir_sha(binary);
    assert([binary writeToFile:compute_binary atomically:YES]);
    NSData *(^compute_json)(void) = ^{ return [NSJSONSerialization dataWithJSONObject:compute_metadata options:0 error:nil]; };
    assert([compute_json() writeToFile:compute_record atomically:YES]);
    assert(shader_compute_load((id)device, path, function));
    device.binary_miss = YES; assert(!shader_compute_load((id)device, path, function)); device.binary_miss = NO;
    compute_metadata[@"os_build"] = @"stale"; assert([compute_json() writeToFile:compute_record atomically:YES]);
    assert(!shader_compute_load((id)device, path, function));
    compute_metadata[@"os_build"] = compute_identity[@"os_build"]; assert([compute_json() writeToFile:compute_record atomically:YES]);
    assert([changed writeToFile:path atomically:YES]); assert(!shader_compute_load((id)device, path, function));
    assert([input writeToFile:path atomically:YES]);
    assert([NSFileManager.defaultManager removeItemAtPath:compute_binary error:nil]);
    assert([NSFileManager.defaultManager createSymbolicLinkAtPath:compute_binary withDestinationPath:outside error:nil]);
    assert(!shader_compute_load((id)device, path, function));
    shader_ir_report("synthetic-cache");
    return 0;
} }
