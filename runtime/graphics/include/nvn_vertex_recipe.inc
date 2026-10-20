#include <objc/runtime.h>
#include "nvn_cube_source.h"
#include "shader_status.h"
static NvShader *zelda_cube_shader(NvShader *base) {
    if (!base || !base->z_cube_uniform) return nil;
    NSString *key = base->z_cube_uniform == 1 ? @"48e3901f9b4eb169_b" : @"c3960b272d349125_b";
    if (strcmp(base->key, key.UTF8String)) return nil;
    static char cache_key;
    NvShader *cached = objc_getAssociatedObject(base, &cache_key);
    if (cached) return cached;
    NSString *path = [g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]];
    NSData *data = [NSData dataWithContentsOfFile:path]; NSError *error = nil;
    if (!data) return nil;
    id<MTLLibrary> library = shader_ir_load(g_dev, path, data.bytes, data.length, true);
    if (!library) {
        NSString *source = zelda_cube_source([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]);
        if (!source) return nil;
        MTLCompileOptions *options = nil; source = zelda_shader_math(source, &options);
        library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:source options:options error:&error]);
    }
    if (!library) { MLOG("Cube pass shader unavailable; retaining single-face passes"); return nil; }
    NvShader *shader = [NvShader new]; shader->fn = [library newFunctionWithName:@"main0"];
    if (!shader->fn) return nil;
    if (base->rf.nb && [shader->fn newArgumentEncoderWithBufferIndex:base->rf.abi].encodedLength != base->arg_len) return nil;
    shader->rf = base->rf; shader->ae = base->ae; shader->arg_len = base->arg_len;
    shader->stage = 0; shader->z_cube_uniform = base->z_cube_uniform; shader->z_layered = 1;
    shader->c1buf = base->c1buf; shader->c1_gpu = base->c1_gpu; shader->c1off = base->c1off;
    strlcpy(shader->key, base->key, sizeof shader->key);
    objc_setAssociatedObject(base, &cache_key, shader, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return shader;
}
#include "shader_status.h"
#include "nvn_wide_line.h"
static NvShader *zelda_line_shader(NvShader *base, NvShader *fragment) {
    // shortcut: only this position-only wave program is proven; add new families with shader and GPU evidence.
    if (!base || !fragment || strcmp(base->key, "0cd9edc0a623c237_b") || strcmp(fragment->key, "e04d541253557805_b")) return nil;
    static char cache_key;
    NvShader *cached = objc_getAssociatedObject(base, &cache_key);
    if (cached) return cached;
    NSString *source = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:@"0cd9edc0a623c237_b.metallib"] encoding:NSUTF8StringEncoding error:nil];
    NSString *fs = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:@"e04d541253557805_b.metallib"] encoding:NSUTF8StringEncoding error:nil];
    if (!zelda_line_source_matches(source, "d701863d2649371dea680b3ed4c2a938f3809f82cee6168704d336ce1a62260d") ||
        !zelda_line_source_matches(fs, "c225af5f8e3719fadf16bca2d62295c9faaf5e9ae02cfea8c90e4b928881e097")) return nil;
    source = zelda_line_source(source);
    MTLCompileOptions *options = nil; source = zelda_shader_math(source, &options); NSError *error = nil;
    id<MTLLibrary> library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:source options:options error:&error]);
    NvShader *shader = [NvShader new]; shader->fn = [library newFunctionWithName:@"main0"];
    if (!shader->fn) MDIE("Wide-line shader compilation failed: %s", error.localizedDescription.UTF8String);
    if ([shader->fn newArgumentEncoderWithBufferIndex:0].encodedLength != base->arg_len)
        MDIE("Wide-line shader changed argument layout");
    shader->rf = base->rf; shader->rf.nv = 0; shader->ae = base->ae; shader->arg_len = base->arg_len;
    shader->stage = 0; shader->range_ro = base->range_ro; shader->z_wide_line = 1;
    shader->c1buf = base->c1buf; shader->c1_gpu = base->c1_gpu; shader->c1off = base->c1off;
    strlcpy(shader->key, base->key, sizeof shader->key);
    objc_setAssociatedObject(base, &cache_key, shader, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return shader;
}
