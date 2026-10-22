#include "shader_prep.h"
#include "shader_ir_source.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_ullong ir_hits, ir_misses, ir_rejected;
static atomic_ullong compute_hits, compute_misses, compute_rejected;

NSData *shader_cache_read(NSString *path, size_t limit) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > limit) { close(fd); return nil; }
    NSFileHandle *file = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
    NSData *data = [file readDataUpToLength:(NSUInteger)st.st_size error:nil];
    [file closeFile];
    return data.length == (NSUInteger)st.st_size ? data : nil;
}

id<MTLLibrary> shader_ir_load(id<MTLDevice> device, NSString *path, const void *source, size_t length, bool layered) {
    NSString *directory = [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"ir-v1"];
    struct stat st;
    if (!device || lstat(directory.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode)) { ir_misses++; return nil; }
    if (!source || !length || length > (64u << 20)) { ir_rejected++; return nil; }
    NSDictionary *expected = shader_ir_source([NSData dataWithBytesNoCopy:(void *)source length:length freeWhenDone:NO], layered, NULL, NULL);
    if (!expected) { ir_rejected++; return nil; }
    NSString *base = [directory stringByAppendingPathComponent:expected[@"key"]];
    NSData *record = shader_cache_read([base stringByAppendingString:@".json"], 64u << 10);
    if (!record) { ir_misses++; return nil; }
    NSDictionary *metadata = [NSJSONSerialization JSONObjectWithData:record options:0 error:nil];
    if (![metadata isKindOfClass:NSDictionary.class]) { ir_rejected++; return nil; }
    for (NSString *key in expected) if (![metadata[key] isEqual:expected[key]]) { ir_rejected++; return nil; }
    NSData *data = shader_cache_read([base stringByAppendingString:@".metallib"], 64u << 20);
    if (data.length < 4 || memcmp(data.bytes, "MTLB", 4) || ![shader_ir_sha(data) isEqual:metadata[@"library_sha256"]]) { ir_rejected++; return nil; }
    dispatch_data_t binary = dispatch_data_create(data.bytes, data.length, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ (void)data; });
    id<MTLLibrary> library = [device newLibraryWithData:binary error:nil];
    if (library) ir_hits++; else ir_rejected++;
    return library;
}

void shader_ir_report(const char *phase) {
    fprintf(stderr, "[shader-ir] %s hits=%llu misses=%llu rejected=%llu\n", phase,
            (unsigned long long)atomic_load(&ir_hits), (unsigned long long)atomic_load(&ir_misses),
            (unsigned long long)atomic_load(&ir_rejected));
    fprintf(stderr, "[compute-binary] %s hits=%llu misses=%llu rejected=%llu\n", phase,
            (unsigned long long)atomic_load(&compute_hits), (unsigned long long)atomic_load(&compute_misses),
            (unsigned long long)atomic_load(&compute_rejected));
}

id<MTLComputePipelineState> shader_compute_load(id<MTLDevice> device, NSString *path, id<MTLFunction> function) {
    NSString *directory = [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"compute-v1"];
    struct stat st;
    if (!device || !function || lstat(directory.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode)) { compute_misses++; return nil; }
    NSDictionary *source = shader_ir_source(shader_cache_read(path, 64u << 20), NO, NULL, NULL);
    NSDictionary *identity = shader_compute_identity(device, source, function.name);
    if (!identity || function.functionType != MTLFunctionTypeKernel) { compute_rejected++; return nil; }
    NSString *base = [directory stringByAppendingPathComponent:identity[@"key"]];
    NSData *record = shader_cache_read([base stringByAppendingString:@".json"], 64u << 10);
    if (!record) { compute_misses++; return nil; }
    NSDictionary *metadata = [NSJSONSerialization JSONObjectWithData:record options:0 error:nil];
    if (![metadata isKindOfClass:NSDictionary.class]) { compute_rejected++; return nil; }
    for (NSString *key in identity) if (![metadata[key] isEqual:identity[key]]) { compute_rejected++; return nil; }
    NSString *binary_path = [base stringByAppendingString:@".metallib"];
    NSData *binary = shader_cache_read(binary_path, 32u << 20);
    if (!binary || ![shader_ir_sha(binary) isEqual:metadata[@"binary_sha256"]]) { compute_rejected++; return nil; }
    MTLBinaryArchiveDescriptor *archive_descriptor = [MTLBinaryArchiveDescriptor new];
    archive_descriptor.url = [NSURL fileURLWithPath:binary_path];
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:archive_descriptor error:nil];
    if (!archive) { compute_rejected++; return nil; }
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function; descriptor.binaryArchives = @[archive];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithDescriptor:descriptor
        options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
    if (pipeline) compute_hits++; else compute_rejected++;
    return pipeline;
}
