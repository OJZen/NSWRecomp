#pragma once
#include <CommonCrypto/CommonDigest.h>
#include "nvn_fetch_record.h"
#ifndef ZELDA_PIPELINE_RECIPE_CONTRACT
#include "pipeline_recipe_identity.h"
#endif

// P1 retains the existing descriptor bytes, plus the reconstruction and source identities they lack.
static NSString *zelda_recipe_key(const char *bytes, size_t size) {
    size_t length = strnlen(bytes, size);
    if (length == size) return nil;
    for (size_t i = length; i < size; i++) if (bytes[i]) return nil;
    NSString *key = [[NSString alloc] initWithBytes:bytes length:length encoding:NSASCIIStringEncoding];
    if (!key) return nil;
    if (!length) return @"";
    NSRegularExpression *pattern = [NSRegularExpression regularExpressionWithPattern:@"^[0-9a-f]{16}(?:-[0-9a-f]{4})?_b$" options:0 error:nil];
    return [pattern numberOfMatchesInString:key options:0 range:NSMakeRange(0, key.length)] == 1 ? key : nil;
}

static NSString *zelda_recipe_source_hash(NSString *directory, NSString *key) {
    if (!key.length) return @"-";
    unsigned char parts[2 * CC_SHA256_DIGEST_LENGTH]; unsigned index = 0;
    for (NSString *suffix in @[@".metallib", @".json"]) {
        NSString *path = [directory stringByAppendingPathComponent:[key stringByAppendingString:suffix]];
        NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
        if (!data.length || data.length > (64u << 20)) return nil;
        CC_SHA256(data.bytes, (CC_LONG)data.length, parts + index++ * CC_SHA256_DIGEST_LENGTH);
    }
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]; char hex[65]; CC_SHA256(parts, sizeof parts, hash);
    for (unsigned i = 0; i < sizeof hash; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    return @(hex);
}

static BOOL zelda_recipe_state(const pso_skey *state) {
    for (unsigned i = 0; i < 16; i++) if (state->k.attr[i].stream >= 16) return NO;
    return zelda_recipe_key(state->vs, sizeof state->vs).length &&
        zelda_recipe_key(state->fs, sizeof state->fs) != nil && state->vs_stage == 0 &&
        (!state->fs[0] || state->fs_stage == 1) && !state->merged && !state->fz &&
        !state->k.vs && !state->k.fs && !state->k.fsfn &&
        (state->k.samples == 0 || state->k.samples == 1 || state->k.samples == 2 ||
         state->k.samples == 4 || state->k.samples == 8 || state->k.samples == 16);
}

static NSString *zelda_pipeline_recipe_line(const pso_skey *state, unsigned vertex, NSString *fetch, NSString *directory) {
    if (vertex > 2 || !zelda_recipe_state(state) || (fetch.length && (!state->fs[0] || !zelda_fetch_record_mapping(fetch)))) return nil;
    NSString *vs = zelda_recipe_key(state->vs, sizeof state->vs), *fs = zelda_recipe_key(state->fs, sizeof state->fs);
    NSString *vh = zelda_recipe_source_hash(directory, vs), *fh = zelda_recipe_source_hash(directory, fs);
    if (!vh || !fh) return nil;
    char hex[sizeof *state * 2 + 1]; pso_skey_hex(state, hex);
    return [NSString stringWithFormat:@"P1\t%s\t%@\t%u\t%@\t%@\t%s\t%@\n",
        ZELDA_PIPELINE_RECIPE_CONTRACT, directory.lastPathComponent, vertex, vh, fh, hex, fetch.length ? fetch : @"-"];
}

// -1 malformed, 0 stale/unavailable, 1 accepted. Untrusted records never select a path outside the source directory.
static int zelda_pipeline_recipe_parse(NSArray<NSString *> *fields, NSString *directory, pso_skey *state,
                                       unsigned *vertex, NSString **fetch) {
    if (fields.count != 8 || ![fields[0] isEqualToString:@"P1"] ||
        ![@[@"0", @"1", @"2"] containsObject:fields[3]] ||
        pso_skey_parse(fields[6].UTF8String, state) || !zelda_recipe_state(state)) return -1;
    NSString *mapping = [fields[7] isEqualToString:@"-"] ? @"" : fields[7];
    if (mapping.length && (!state->fs[0] || !zelda_fetch_record_mapping(mapping))) return -1;
    if (![fields[1] isEqualToString:@ZELDA_PIPELINE_RECIPE_CONTRACT] ||
        ![fields[2] isEqualToString:directory.lastPathComponent]) return 0;
    NSString *vs = zelda_recipe_key(state->vs, sizeof state->vs), *fs = zelda_recipe_key(state->fs, sizeof state->fs);
    if (![fields[4] isEqualToString:zelda_recipe_source_hash(directory, vs)] ||
        ![fields[5] isEqualToString:zelda_recipe_source_hash(directory, fs)]) return 0;
    *vertex = fields[3].intValue; *fetch = mapping;
    return 1;
}
