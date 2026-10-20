#include "shader_status.h"
// A fragment shader may write depth while the pass has no depth attachment (no depth target, or stencil only).
// NVN discards that output; Metal rejects the pipeline ("shader writes to depth"). Such pipelines use a copy of the
// fragment function without the depth output. Every Zelda depth write is the SPIR-V-Cross pair
// `float gl_FragDepth [[depth(any)]];` / `out.gl_FragDepth = value;` (73 of the shipped fragment shaders).
#import <objc/runtime.h>

static NSString *zelda_depthless_source(NSString *source) {
    if (!source) return nil;
    NSMutableString *result = [source mutableCopy];
    NSRegularExpression *member = [NSRegularExpression regularExpressionWithPattern:@"^[ \\t]*float gl_FragDepth \\[\\[depth\\(\\w+\\)\\]\\];\\n"
        options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSRegularExpression *store = [NSRegularExpression regularExpressionWithPattern:@"\\bout\\.gl_FragDepth = ([^;\\n]+);" options:0 error:nil];
    if ([member replaceMatchesInString:result options:0 range:NSMakeRange(0, result.length) withTemplate:@""] != 1) return nil;
    [store replaceMatchesInString:result options:0 range:NSMakeRange(0, result.length) withTemplate:@"(void)($1);"];
    if ([result rangeOfString:@"gl_FragDepth"].location != NSNotFound || [result rangeOfString:@"[[depth("].location != NSNotFound) return nil;
    return result;
}

#ifndef ZELDA_DEPTH_OUTPUT_ANALYSIS_ONLY
// Loaded functions remember their source file, runtime-built ones their text, so a depth-free copy can be compiled later.
static char z_function_path, z_function_text;
static void zelda_function_source(id<MTLFunction> function, NSString *path, NSString *text) {
    if (!function) return;
    if (path) function.label = path.lastPathComponent.stringByDeletingPathExtension;
    if (path) objc_setAssociatedObject(function, &z_function_path, path, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (text) objc_setAssociatedObject(function, &z_function_text, text, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static NSString *zelda_function_text(id<MTLFunction> function) {
    NSString *text = objc_getAssociatedObject(function, &z_function_text);
    if (text) return text;
    NSString *path = objc_getAssociatedObject(function, &z_function_path);
    return path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
}
static os_unfair_lock z_depthless_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *z_depthless;   // function -> depth-free copy, or NSNull when it writes no depth
static uint64_t z_depthless_functions;
static id zelda_depthless_lookup(id<MTLFunction> function, NvShader *shader) {
    if (!z_depthless) z_depthless = [NSMapTable strongToStrongObjectsMapTable];
    id cached = [z_depthless objectForKey:function];
    if (cached) return cached;
    NSString *text = zelda_function_text(function);
    cached = NSNull.null;
    if (text && [text rangeOfString:@"[[depth("].location != NSNotFound) {
        NSString *source = zelda_depthless_source(text);
        if (!source) MDIE("Depth output of fragment shader %s could not be removed", shader ? shader->key : "?");
        MTLCompileOptions *options = nil; source = zelda_shader_math(source, &options); NSError *error = nil;
        id<MTLLibrary> library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:source options:options error:&error]);
        id<MTLFunction> copy = library ? [library newFunctionWithName:function.name] : nil;
        if (!copy) MDIE("Depth-free fragment shader %s failed: %s", shader ? shader->key : "?", error.localizedDescription.UTF8String);
        copy.label = [NSString stringWithFormat:@"%@ depthless", function.label ?: function.name];
        if (shader && shader->rf.nb && [copy newArgumentEncoderWithBufferIndex:shader->rf.abi].encodedLength != shader->arg_len)
            MDIE("Depth-free fragment shader changed the argument buffer layout");
        cached = copy; z_depthless_functions++;
        MLOG("Depthless pass: removed depth output from fragment shader %s (variant %llu)", shader ? shader->key : "?", z_depthless_functions);
    }
    [z_depthless setObject:cached forKey:function];
    return cached;
}
static int zelda_function_writes_depth(id<MTLFunction> function, NvShader *shader) {
    if (!function) return 0;
    os_unfair_lock_lock(&z_depthless_lock);
    id cached = zelda_depthless_lookup(function, shader);
    os_unfair_lock_unlock(&z_depthless_lock);
    return cached != NSNull.null;
}
// The fragment function of a pipeline whose pass has no depth attachment.
static id<MTLFunction> zelda_depthless_function(id<MTLFunction> function, NvShader *shader) {
    if (!function) return function;
    os_unfair_lock_lock(&z_depthless_lock);
    id cached = zelda_depthless_lookup(function, shader);
    os_unfair_lock_unlock(&z_depthless_lock);
    return cached == NSNull.null ? function : cached;
}
#endif
