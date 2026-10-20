#pragma once
#include <string.h>

static int zelda_fetch_record_key(NSString *key) {
    return key.length == 18 && [key hasSuffix:@"_b"] && strspn(key.UTF8String, "0123456789abcdef") == 16;
}

static NSString *zelda_fetch_record_identity(NSDictionary<NSString *, NSNumber *> *mapping) {
    NSMutableString *identity = [NSMutableString string];
    for (NSString *name in [mapping.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [identity appendFormat:@"%@=c%d;", name, mapping[name].intValue];
    return identity;
}

// Persistent records are input: reject partial names, duplicate bindings and out-of-range attachment indices.
static NSDictionary<NSString *, NSNumber *> *zelda_fetch_record_mapping(NSString *identity) {
    if (!identity.length || identity.length > 2048) return nil;
    static NSRegularExpression *field;
    if (!field) field = [NSRegularExpression regularExpressionWithPattern:@"([A-Za-z_][A-Za-z0-9_]{0,127})=c([0-7]);" options:0 error:nil];
    NSArray<NSTextCheckingResult *> *matches = [field matchesInString:identity options:0 range:NSMakeRange(0, identity.length)];
    if (!matches.count || matches.count > 32) return nil;
    NSMutableDictionary *mapping = [NSMutableDictionary dictionary]; NSUInteger offset = 0;
    for (NSTextCheckingResult *match in matches) {
        if (match.range.location != offset) return nil;
        NSString *name = [identity substringWithRange:[match rangeAtIndex:1]];
        if (mapping[name]) return nil;
        mapping[name] = @([[identity substringWithRange:[match rangeAtIndex:2]] intValue]);
        offset = NSMaxRange(match.range);
    }
    return offset == identity.length && [zelda_fetch_record_identity(mapping) isEqualToString:identity] ? mapping : nil;
}
