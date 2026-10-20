#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>

typedef struct {
    uint64_t vertices, indices, vertex_bytes;
    uint32_t stride, offset, first, count;
    int32_t base_vertex;
    uint32_t index_size, primitive, width;
    float viewport[2];
    uint32_t restart_index, restart, vertex_size, padding;
} zelda_line_params;
_Static_assert(sizeof(zelda_line_params) == 80, "Wide-line GPU parameter layout");

static int zelda_line_source_matches(NSString *source, const char *expected) {
    NSData *data = [source dataUsingEncoding:NSUTF8StringEncoding];
    if (!data || data.length > 262144) return 0;
    uint8_t digest[32]; char hex[65]; CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    return !strcmp(hex, expected);
}

static NSString *zelda_line_source(NSString *input) {
    NSString *source = input;
    NSString *entry = @"vertex main0_out main0(constant float& zelda_point_size [[buffer(15)]], main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])";
    if ([source componentsSeparatedByString:entry].count != 2) return nil;
    source = [source stringByReplacingOccurrencesOfString:@"    float gl_PointSize [[point_size]];" withString:@""];
    source = [source stringByReplacingOccurrencesOfString:@"    out.gl_PointSize = zelda_point_size;" withString:@""];
    source = [source stringByReplacingOccurrencesOfString:entry withString:@"static main0_out zelda_line_position(main0_in in, constant spvDescriptorSetBuffer0& spvDescriptorSet0)"];
    // Aliased wide lines replicate coverage along the minor axis; the original position math is unchanged.
    return [source stringByAppendingString:@"\n"
"struct ZeldaLineParams {\n"
"    device const uchar *vertices;\n"
"    device const uchar *indices;\n"
"    ulong vertex_bytes;\n"
"    uint stride, offset, first, count;\n"
"    int base_vertex;\n"
"    uint index_size, primitive, width;\n"
"    float2 viewport;\n"
"    uint restart_index, restart, vertex_size, padding;\n"
"};\n"
"static bool zelda_line_x(constant ZeldaLineParams& p, uint n, thread float& x) {\n"
"    uint index = n + p.first;\n"
"    if (p.index_size == 1) index = p.indices[n];\n"
"    else if (p.index_size == 2) index = ((device const ushort*)p.indices)[n];\n"
"    else if (p.index_size == 4) index = ((device const uint*)p.indices)[n];\n"
"    if (p.index_size && p.restart && index == p.restart_index) return false;\n"
"    long vertex_index = long(index) + p.base_vertex;\n"
"    if (vertex_index < 0) return false;\n"
"    ulong address = ulong(vertex_index) * p.stride + p.offset;\n"
"    if (address + p.vertex_size > p.vertex_bytes) return false;\n"
"    x = p.vertex_size == 1 ? float(p.vertices[address]) / 255.0 : *((device const float*)(p.vertices + address));\n"
"    return true;\n"
"}\n"
"vertex main0_out main0(uint v [[vertex_id]], uint instance [[instance_id]],\n"
"    constant ZeldaLineParams& p [[buffer(14)]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]) {\n"
"    main0_out out = {};\n"
"    out.gl_Position = float4(2, 2, 0.5, 1);\n"
"    uint segments = p.primitive == 1 ? p.count / 2 : p.count - 1;\n"
"    uint segment = instance % segments;\n"
"    uint first = p.primitive == 1 ? segment * 2 : segment;\n"
"    float a, b;\n"
"    if (!zelda_line_x(p, first, a) || !zelda_line_x(p, first + 1, b)) return out;\n"
"    float4 start = zelda_line_position(main0_in{float4(a, 0, 0, 1)}, spvDescriptorSet0).gl_Position;\n"
"    float4 end = zelda_line_position(main0_in{float4(b, 0, 0, 1)}, spvDescriptorSet0).gl_Position;\n"
"    float2 delta = abs((end.xy / end.w - start.xy / start.w) * p.viewport);\n"
"    uint axis = delta.x >= delta.y ? 1 : 0;\n"
"    out.gl_Position = (v & 1) ? end : start;\n"
"    float shift = float(v / 2) - float(p.width - 1) * 0.5;\n"
"    out.gl_Position[axis] += (2.0 * shift / p.viewport[axis]) * out.gl_Position.w;\n"
"    return out;\n"
"}\n"];
}
