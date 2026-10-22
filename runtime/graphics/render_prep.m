#include "shader_prep.h"
#include "shader_ir_source.h"
#include <stdatomic.h>
#include <sys/stat.h>

static atomic_ullong render_hits, render_misses, render_rejected;

NSDictionary *shader_render_context(id<MTLDevice> device, NSString *directory, NSString *contract) {
    if (!device.architecture.name.length || !directory.length || !contract.length) return nil;
    return @{@"format":@"totk-render-binary-v1", @"architecture":device.architecture.name,
             @"device":@(device.registryID), @"os_build":NSProcessInfo.processInfo.operatingSystemVersionString,
             @"chain":directory.lastPathComponent, @"contract":contract};
}

static BOOL render_keys_valid(id keys) {
    if (![keys isKindOfClass:NSArray.class] || ![keys count] || [keys count] > 32) return NO;
    NSMutableSet *seen = [NSMutableSet set];
    NSCharacterSet *bad = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    for (id key in keys) {
        if (![key isKindOfClass:NSString.class] || [key length] != 64 ||
            [key rangeOfCharacterFromSet:bad].location != NSNotFound || [seen containsObject:key]) return NO;
        [seen addObject:key];
    }
    return YES;
}

static BOOL render_error(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"TOTKRenderPreparation" code:78 userInfo:@{NSLocalizedDescriptionKey:message}];
    return NO;
}

BOOL shader_render_capture(id<MTLDevice> device, NSArray<MTLRenderPipelineDescriptor *> *descriptors,
                           NSArray<NSString *> *keys, NSString *directory, NSString *contract, NSString *output, NSError **error) {
    NSDictionary *context = shader_render_context(device, directory, contract);
    if (!context || !render_keys_valid(keys) || descriptors.count != keys.count) return render_error(error, @"Expected 1 to 32 unique render descriptors");
    NSString *root = directory.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    NSString *path = output.stringByStandardizingPath;
    struct stat st;
    if (!path.isAbsolutePath || ![path hasPrefix:[root stringByAppendingString:@"/"]] ||
        ![path isEqual:path.stringByResolvingSymlinksInPath] || !lstat(path.fileSystemRepresentation, &st))
        return render_error(error, @"Render capture requires a new directory under work");
    if (![NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:NO attributes:nil error:error]) return NO;
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:error];
    if (!archive) return NO;
    for (MTLRenderPipelineDescriptor *descriptor in descriptors) {
        if (!descriptor.vertexFunction || descriptor.binaryArchives.count ||
            ![archive addRenderPipelineFunctionsWithDescriptor:descriptor error:error]) return NO;
    }
    NSString *partial = [path stringByAppendingPathComponent:@"capture.partial"];
    if (![archive serializeToURL:[NSURL fileURLWithPath:partial] error:error]) return NO;
    NSData *binary = shader_cache_read(partial, 32u << 20);
    if (!binary) {
        [NSFileManager.defaultManager removeItemAtPath:partial error:nil];
        return render_error(error, @"Render capture exceeds its 32 MiB bound or is unreadable");
    }
    NSMutableDictionary *manifest = [context mutableCopy]; manifest[@"format"] = @"totk-render-capture-v1";
    manifest[@"keys"] = keys; manifest[@"binary_sha256"] = shader_ir_sha(binary);
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingSortedKeys error:error];
    if (!json || ![NSFileManager.defaultManager moveItemAtPath:partial toPath:[path stringByAppendingPathComponent:@"capture.metallib"] error:error]) return NO;
    return [json writeToFile:[path stringByAppendingPathComponent:@"capture.json"] options:NSDataWritingAtomic error:error];
}

id<MTLRenderPipelineState> shader_render_load(id<MTLDevice> device, MTLRenderPipelineDescriptor *descriptor,
                                             NSString *key, NSString *directory, NSString *contract) {
    if (!key.length) return nil;
    static NSDictionary *loaded_context; static NSString *loaded_directory;
    static NSSet<NSString *> *keys; static id<MTLBinaryArchive> archive; static dispatch_once_t once;
    NSDictionary *expected = shader_render_context(device, directory, contract);
    if (!expected) { render_rejected++; return nil; }
    dispatch_once(&once, ^{
        loaded_context = expected; loaded_directory = directory;
        NSString *cache = [directory stringByAppendingPathComponent:@"render-v1"]; struct stat st;
        if (lstat(cache.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode)) return;
        NSData *record = shader_cache_read([cache stringByAppendingPathComponent:@"manifest.json"], 64u << 10);
        NSDictionary *manifest = record ? [NSJSONSerialization JSONObjectWithData:record options:0 error:nil] : nil;
        if (![manifest isKindOfClass:NSDictionary.class] || !render_keys_valid(manifest[@"keys"])) { render_rejected++; return; }
        for (NSString *field in expected) if (![manifest[field] isEqual:expected[field]]) { render_rejected++; return; }
        NSString *path = [cache stringByAppendingPathComponent:@"pipelines.metallib"];
        NSData *binary = shader_cache_read(path, 32u << 20);
        if (!binary || ![shader_ir_sha(binary) isEqual:manifest[@"binary_sha256"]]) { render_rejected++; return; }
        MTLBinaryArchiveDescriptor *ad = [MTLBinaryArchiveDescriptor new]; ad.url = [NSURL fileURLWithPath:path];
        archive = [device newBinaryArchiveWithDescriptor:ad error:nil];
        if (!archive) { render_rejected++; return; }
        keys = [NSSet setWithArray:manifest[@"keys"]];
    });
    if (![loaded_context isEqual:expected] || ![loaded_directory isEqual:directory]) { render_rejected++; return nil; }
    if (!archive || ![keys containsObject:key]) { render_misses++; return nil; }
    // The key selects candidates; Metal validates the complete descriptor and function binaries.
    MTLRenderPipelineDescriptor *copy = [descriptor copy]; copy.binaryArchives = @[archive];
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:copy
        options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
    if (pipeline) render_hits++; else render_rejected++;
    return pipeline;
}

void shader_render_report(void) {
    fprintf(stderr, "[render-binary] hits=%llu misses=%llu rejected=%llu\n",
            (unsigned long long)atomic_load(&render_hits), (unsigned long long)atomic_load(&render_misses),
            (unsigned long long)atomic_load(&render_rejected));
}
