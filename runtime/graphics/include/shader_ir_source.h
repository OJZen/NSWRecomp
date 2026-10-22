#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <stdio.h>
#include "nvn_shader_math.h"
#include "nvn_cube_source.h"

static NSString *shader_ir_sha(NSData *data) {
    if (!data || data.length > UINT32_MAX) return nil;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]; char hex[65];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    for (unsigned i = 0; i < sizeof hash; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return @(hex);
}

// Both preparation and loading derive identity from the exact transformed text and compiler options.
static NSDictionary *shader_ir_source(NSData *input, BOOL layered, NSString **text, MTLCompileOptions **options) {
    if (!input.length || input.length > (64u << 20)) return nil;
    NSString *source = [[NSString alloc] initWithData:input encoding:NSUTF8StringEncoding];
    if (!source) return nil;
    if (layered) source = zelda_cube_source(source);
    if (!source) return nil;
    MTLCompileOptions *math = nil; source = zelda_shader_math(source, &math);
    if (!math) math = [MTLCompileOptions new];
    if ((math.mathMode != MTLMathModeFast && math.mathMode != MTLMathModeRelaxed && math.mathMode != MTLMathModeSafe) ||
        (math.mathFloatingPointFunctions != MTLMathFloatingPointFunctionsFast && math.mathFloatingPointFunctions != MTLMathFloatingPointFunctionsPrecise)) return nil;
    NSString *mode = math.mathMode == MTLMathModeSafe ? @"safe" : math.mathMode == MTLMathModeRelaxed ? @"relaxed" : @"fast";
    NSString *fp32 = math.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsPrecise ? @"precise" : @"fast";
    NSUInteger language = math.languageVersion;
    NSString *standard = (language >> 16) >= 3 ? @"metal" : @"macos-metal";
    NSArray *flags = @[[NSString stringWithFormat:@"-std=%@%lu.%lu", standard, (unsigned long)(language >> 16), (unsigned long)(language & 65535)],
                       [@"-fmetal-math-mode=" stringByAppendingString:mode], [@"-fmetal-math-fp32-functions=" stringByAppendingString:fp32]];
    NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
    NSMutableDictionary *description = [@{@"format":@"totk-metal-ir-v1", @"layered":@(layered), @"flags":flags,
        @"os":[NSString stringWithFormat:@"%ld.%ld.%ld", (long)os.majorVersion, (long)os.minorVersion, (long)os.patchVersion],
        @"source_sha256":shader_ir_sha(input), @"prepared_sha256":shader_ir_sha([source dataUsingEncoding:NSUTF8StringEncoding])} mutableCopy];
    description[@"key"] = shader_ir_sha([NSJSONSerialization dataWithJSONObject:description options:NSJSONWritingSortedKeys error:nil]);
    if (text) *text = source;
    if (options) *options = math;
    return description;
}

// This identity is only for the default compute descriptor used by guest compute dispatch and warm-up.
static NSDictionary *shader_compute_identity(id<MTLDevice> device, NSDictionary *source, NSString *function) {
    if (!device || !source || !function.length || !device.architecture.name.length) return nil;
    NSMutableDictionary *identity = [@{@"format":@"totk-compute-binary-v1", @"ir_key":source[@"key"],
        @"function":function, @"architecture":device.architecture.name, @"device":@(device.registryID),
        @"os_build":NSProcessInfo.processInfo.operatingSystemVersionString} mutableCopy];
    identity[@"key"] = shader_ir_sha([NSJSONSerialization dataWithJSONObject:identity options:NSJSONWritingSortedKeys error:nil]);
    return identity;
}
