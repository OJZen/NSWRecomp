#include "shader_status.h"
// Fullscreen render passes as compute dispatches.
// v1.4.3 spends most of each frame's GPU time on fixed per-pass costs: the campfire scene opens about 186 render
// passes, roughly 75 of them a single fullscreen draw into a small target (blur and downsample chains, mip
// generation). A dependent chain of such passes costs 38-57 us per step on an M4 whatever the target size up to
// 800x448, while dispatches in one compute encoder cost 2-9 us and a compute encoder each 14-28 us (Metal 4
// microbenchmark). Merging the draws into one render encoder with fragment-to-fragment
// barriers is not an option: those barriers do not order work across tiles and the chain reads stale texels.
// A draw that would open a render pass on its own is therefore run as two dispatches in the shared copy/compute
// encoder:
//   1. a setup kernel made from the vertex shader fetches the vertex attributes and stores each vertex's clip
//      position and outputs (by location);
//   2. a kernel made from the fragment shader rasterizes the one or two triangles at every pixel of the scissored
//      target with homogeneous edge functions (viewport, depth range, depth clip, cull mode and winding as the draw
//      sets them), interpolates the inputs as declared (perspective, no_perspective, flat), runs the fragment body and
//      writes each colour output with texture.write. Threads are laid out as 2x2 quads so dfdx / dfdy and the
//      implicit-LOD samples (rewritten to gradient samples) see the same neighbours as hardware quads.
// Only draws whose result does not depend on anything a kernel cannot reproduce qualify: no blending, full channel
// masks, single sample, no depth or stencil test, no pending clears or queries, no side effects in the fragment
// shader, targets of at most 800x448 (larger passes are bandwidth bound and lose lossless compression to ShaderWrite).
// ZELDA_FS_COMPUTE=1 enables it.
#import <objc/runtime.h>

// ---------- Part 1: MSL rewrite (also compiled by tests/test_nvn_fullscreen_compute.m) ----------

enum { ZFC_LOCATIONS = 16, ZFC_SLOT_FETCH = 28, ZFC_SLOT_PARAMS = 29, ZFC_SLOT_VERTICES = 30 };
// Window winding: the kernel computes the signed area in window coordinates (y down); positive is clockwise there.
static NSString *const zelda_fsc_common = @R"MSL(
struct ZfcVert { float4 pos; uint4 loc[16]; };
struct ZfcAttr { const device uchar *ptr; uint stride; uint fmt; };
struct ZfcFetch { ZfcAttr a[16]; uint vid[8]; uint count; uint base_instance; uint clip; uint pad; device atomic_uint *clipped; device ulong *samples; };
struct ZfcParams { float vp[4]; float dr[2]; uint org[2]; uint lo[2]; uint hi[2]; uint ntri; uint cull; uint ccw; uint clip; uint tri[6]; device atomic_uint *samples; };
struct ZfcHit { bool covered; uint t; float3 b; float3 l; float z; float iw; bool front; };

static inline uint zfc_b(const device uchar *p, uint o) { return uint(p[o]); }
static inline uint zfc_h(const device uchar *p, uint o) { return uint(p[o]) | uint(p[o + 1]) << 8; }
static inline uint zfc_w(const device uchar *p, uint o) { return uint(p[o]) | uint(p[o + 1]) << 8 | uint(p[o + 2]) << 16 | uint(p[o + 3]) << 24; }
static inline float zfc_sn8(uint v) { return max(float(int(v << 24) >> 24) / 127.0f, -1.0f); }
static inline float zfc_sn16(uint v) { return max(float(int(v << 16) >> 16) / 32767.0f, -1.0f); }
// Vertex fetch by MTLVertexFormat, missing components (0, 0, 0, 1) as the fixed-function fetch fills them.
static float4 zfc_fetch(constant ZfcAttr &a, uint index) {
    float4 r = float4(0, 0, 0, 1);
    if (!a.fmt) return r;
    const device uchar *p = a.ptr + index * a.stride;
    uint f = a.fmt, n = 0;
    if (f >= 28 && f <= 31) { n = f - 27; for (uint i = 0; i < n; i++) r[i] = as_type<float>(zfc_w(p, 4 * i)); }
    else if (f == 53 || (f >= 25 && f <= 27)) { n = f == 53 ? 1 : f - 23; for (uint i = 0; i < n; i++) r[i] = float(as_type<half>(ushort(zfc_h(p, 2 * i)))); }
    else if (f == 47 || (f >= 7 && f <= 9)) { n = f == 47 ? 1 : f - 5; for (uint i = 0; i < n; i++) r[i] = float(zfc_b(p, i)) / 255.0f; }
    else if (f == 42) { r = float4(zfc_b(p, 2), zfc_b(p, 1), zfc_b(p, 0), zfc_b(p, 3)) / 255.0f; }
    else if (f == 48 || (f >= 10 && f <= 12)) { n = f == 48 ? 1 : f - 8; for (uint i = 0; i < n; i++) r[i] = zfc_sn8(zfc_b(p, i)); }
    else if (f == 51 || (f >= 19 && f <= 21)) { n = f == 51 ? 1 : f - 17; for (uint i = 0; i < n; i++) r[i] = float(zfc_h(p, 2 * i)) / 65535.0f; }
    else if (f == 52 || (f >= 22 && f <= 24)) { n = f == 52 ? 1 : f - 20; for (uint i = 0; i < n; i++) r[i] = zfc_sn16(zfc_h(p, 2 * i)); }
    else if (f == 41) { uint v = zfc_w(p, 0); r = float4(v & 1023, v >> 10 & 1023, v >> 20 & 1023, v >> 30) / float4(1023, 1023, 1023, 3); }
    else if (f == 40) { int v = int(zfc_w(p, 0)); r = max(float4(v << 22 >> 22, v << 12 >> 22, v << 2 >> 22, v >> 30) / float4(511, 511, 511, 1), -1.0f); }
    else if (f == 45 || (f >= 1 && f <= 3)) { n = f == 45 ? 1 : f + 1; for (uint i = 0; i < n; i++) r[i] = float(zfc_b(p, i)); }
    else if (f == 46 || (f >= 4 && f <= 6)) { n = f == 46 ? 1 : f - 2; for (uint i = 0; i < n; i++) r[i] = float(int(zfc_b(p, i) << 24) >> 24); }
    else if (f == 49 || (f >= 13 && f <= 15)) { n = f == 49 ? 1 : f - 11; for (uint i = 0; i < n; i++) r[i] = float(zfc_h(p, 2 * i)); }
    else if (f == 50 || (f >= 16 && f <= 18)) { n = f == 50 ? 1 : f - 14; for (uint i = 0; i < n; i++) r[i] = float(int(zfc_h(p, 2 * i) << 16) >> 16); }
    else if (f >= 36 && f <= 39) { n = f - 35; for (uint i = 0; i < n; i++) r[i] = float(zfc_w(p, 4 * i)); }
    else if (f >= 32 && f <= 35) { n = f - 31; for (uint i = 0; i < n; i++) r[i] = float(int(zfc_w(p, 4 * i))); }
    return r;
}
static inline uint4 zfc_pack(float v) { return as_type<uint4>(float4(v, 0, 0, 0)); }
static inline uint4 zfc_pack(float2 v) { return as_type<uint4>(float4(v, 0, 0)); }
static inline uint4 zfc_pack(float3 v) { return as_type<uint4>(float4(v, 0)); }
static inline uint4 zfc_pack(float4 v) { return as_type<uint4>(v); }
static inline uint4 zfc_pack(half v) { return zfc_pack(float(v)); }
static inline uint4 zfc_pack(half2 v) { return zfc_pack(float2(v)); }
static inline uint4 zfc_pack(half3 v) { return zfc_pack(float3(v)); }
static inline uint4 zfc_pack(half4 v) { return zfc_pack(float4(v)); }
static inline uint4 zfc_pack(int v) { return uint4(as_type<uint>(v), 0, 0, 0); }
static inline uint4 zfc_pack(int2 v) { return uint4(as_type<uint2>(v), 0, 0); }
static inline uint4 zfc_pack(int3 v) { return uint4(as_type<uint3>(v), 0); }
static inline uint4 zfc_pack(int4 v) { return as_type<uint4>(v); }
static inline uint4 zfc_pack(uint v) { return uint4(v, 0, 0, 0); }
static inline uint4 zfc_pack(uint2 v) { return uint4(v, 0, 0); }
static inline uint4 zfc_pack(uint3 v) { return uint4(v, 0); }
static inline uint4 zfc_pack(uint4 v) { return v; }
static inline float4 zfc_out(float v) { return float4(v, 0, 0, 1); }
static inline float4 zfc_out(float2 v) { return float4(v, 0, 1); }
static inline float4 zfc_out(float3 v) { return float4(v, 1); }
static inline float4 zfc_out(float4 v) { return v; }
static inline float4 zfc_out(half v) { return zfc_out(float(v)); }
static inline float4 zfc_out(half2 v) { return zfc_out(float2(v)); }
static inline float4 zfc_out(half3 v) { return zfc_out(float3(v)); }
static inline float4 zfc_out(half4 v) { return float4(v); }
// Derivatives across a 2x2 quad: lanes 0 1 / 2 3 of each group of four threads.
template<typename T> static inline T zfc_dx(T v, ushort l) { T o = quad_shuffle_xor(v, ushort(1)); return (l & 1) ? v - o : o - v; }
template<typename T> static inline T zfc_dy(T v, ushort l) { T o = quad_shuffle_xor(v, ushort(2)); return (l & 2) ? v - o : o - v; }
template<typename T> static inline T zfc_fw(T v, ushort l) { return abs(zfc_dx(v, l)) + abs(zfc_dy(v, l)); }
static inline float zfc_cross(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
// Pixel centre p against triangle t: 2D homogeneous edge functions in window space, so vertices behind the eye and
// outside the viewport need no clipping. b: perspective-correct barycentrics, l: screen-linear; both extrapolate
// outside the triangle (helper lanes of a partly covered quad).
static ZfcHit zfc_triangle(constant ZfcParams &zp, const device ZfcVert *zv, float2 p, uint t) {
    ZfcHit h; h.t = t;
    float hw = zp.vp[2] * 0.5f, hh = zp.vp[3] * 0.5f, cx = zp.vp[0] + hw, cy = zp.vp[1] + hh;
    float4 c0 = zv[zp.tri[3 * t]].pos, c1 = zv[zp.tri[3 * t + 1]].pos, c2 = zv[zp.tri[3 * t + 2]].pos;
    float3 a0 = float3(cx * c0.w + hw * c0.x, cy * c0.w - hh * c0.y, c0.w);
    float3 a1 = float3(cx * c1.w + hw * c1.x, cy * c1.w - hh * c1.y, c1.w);
    float3 a2 = float3(cx * c2.w + hw * c2.x, cy * c2.w - hh * c2.y, c2.w);
    float3 P = float3(p, 1);
    float D = dot(a0, cross(a1, a2));
    float3 d = float3(dot(P, cross(a1, a2)), dot(a0, cross(P, a2)), dot(a0, cross(a1, P)));
    float sum = d.x + d.y + d.z;
    h.b = sum != 0 ? d / sum : float3(1, 0, 0);
    // Values are interpolated as v0 + b1 (v1 - v0) + b2 (v2 - v0): exact when the vertices agree (a fullscreen pass at
    // depth 1 must not lose pixels to a sum of barycentrics just above 1).
    float3 w = float3(c0.w, c1.w, c2.w);
    float bw = w.x + h.b.y * (w.y - w.x) + h.b.z * (w.z - w.x);
    h.l = bw != 0 && (w.y != w.x || w.z != w.x) ? h.b * w / bw : h.b;
    h.iw = bw != 0 ? 1 / bw : 1;
    float3 zn = float3(c0.z, c1.z, c2.z) / w;
    float z = zn.x + h.l.y * (zn.y - zn.x) + h.l.z * (zn.z - zn.x);
    h.z = zp.dr[0] + z * (zp.dr[1] - zp.dr[0]);
    h.front = (D > 0) != (zp.ccw != 0);
    h.covered = false;
    if (D == 0 || (zp.cull == 1 && h.front) || (zp.cull == 2 && !h.front)) return h;
    float s = D > 0 ? 1 : -1;
    bool in;
    if (c0.w > 0 && c1.w > 0 && c2.w > 0) {
        // In front of the eye: edge functions on window positions snapped to the rasterizer's 1/256-pixel grid
        // (exact in single precision for targets this size), top-left rule for centres on an edge (clockwise order
        // in y-down coordinates: top = horizontal going right, left = going up).
        float2 v[3] = {rint(a0.xy / a0.z * 256) / 256, rint(a1.xy / a1.z * 256) / 256, rint(a2.xy / a2.z * 256) / 256};
        float area = zfc_cross(v[1] - v[0], v[2] - v[0]);
        float sa = area > 0 ? 1 : -1;
        in = area != 0;
        for (uint i = 0; i < 3 && in; i++) {
            float2 from = v[(i + 1) % 3], e = (v[(i + 2) % 3] - from) * sa;
            float f = zfc_cross(e, p - from);
            in = f > 0 || (f == 0 && ((e.y == 0 && e.x > 0) || e.y < 0));
        }
    } else in = d.x * s >= 0 && d.y * s >= 0 && d.z * s >= 0 && sum * s > 0;
    h.covered = in && !(zp.clip && (z < 0 || z > 1));
    return h;
}
static inline float4 zfc_interpolate(const device ZfcVert *zv, constant ZfcParams &zp, uint t, float3 b, uint L) {
    float4 a0 = as_type<float4>(zv[zp.tri[3 * t]].loc[L]), a1 = as_type<float4>(zv[zp.tri[3 * t + 1]].loc[L]), a2 = as_type<float4>(zv[zp.tri[3 * t + 2]].loc[L]);
    return a0 + b.y * (a1 - a0) + b.z * (a2 - a0);
}
static inline float4 zfc_perspective(const device ZfcVert *zv, constant ZfcParams &zp, ZfcHit h, uint L) { return zfc_interpolate(zv, zp, h.t, h.b, L); }
static inline float4 zfc_linear(const device ZfcVert *zv, constant ZfcParams &zp, ZfcHit h, uint L) { return zfc_interpolate(zv, zp, h.t, h.l, L); }
static inline uint4 zfc_flat(const device ZfcVert *zv, constant ZfcParams &zp, ZfcHit h, uint L) { return zv[zp.tri[3 * h.t]].loc[L]; }
)MSL";

// Index of the bracket closing the one at `open`, or NSNotFound.
static NSUInteger zelda_fsc_close(NSString *s, NSUInteger open) {
    unichar o = [s characterAtIndex:open], c = o == '(' ? ')' : o == '{' ? '}' : o == '[' ? ']' : '>';
    int depth = 0;
    for (NSUInteger i = open; i < s.length; i++) {
        unichar ch = [s characterAtIndex:i];
        if (ch == o) depth++;
        else if (ch == c && --depth == 0) return i;
    }
    return NSNotFound;
}
// Splits at top-level commas; angle brackets count as nesting only in declarations.
static NSArray<NSString *> *zelda_fsc_split(NSString *s, int angles) {
    NSMutableArray *parts = [NSMutableArray array];
    int depth = 0; NSUInteger start = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar ch = [s characterAtIndex:i];
        if (ch == '(' || ch == '[' || (angles && ch == '<')) depth++;
        else if (ch == ')' || ch == ']' || (angles && ch == '>')) depth--;
        else if (ch == ',' && depth == 0) { [parts addObject:[[s substringWithRange:NSMakeRange(start, i - start)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]]; start = i + 1; }
    }
    NSString *last = [[s substringFromIndex:start] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (last.length) [parts addObject:last];
    return parts;
}
static NSString *zelda_fsc_group(NSString *pattern, NSString *text, NSUInteger group) {
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:nil];
    NSTextCheckingResult *m = [re firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
    return m && [m rangeAtIndex:group].location != NSNotFound ? [text substringWithRange:[m rangeAtIndex:group]] : nil;
}
// Members of `struct name { ... };` as {type, name, attr}.
static NSArray<NSDictionary *> *zelda_fsc_members(NSString *source, NSString *name) {
    NSRange r = [source rangeOfString:[NSString stringWithFormat:@"struct %@\n{", name]];
    if (r.location == NSNotFound) return nil;
    NSUInteger open = NSMaxRange(r) - 1, close = zelda_fsc_close(source, open);
    if (close == NSNotFound) return nil;
    NSMutableArray *members = [NSMutableArray array];
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^\\s*([A-Za-z_][\\w<>:, ]*?)\\s+(\\w+)\\s*(\\[\\[([^\\]]*)\\]\\])?\\s*;"
                                                                        options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSString *body = [source substringWithRange:NSMakeRange(open + 1, close - open - 1)];
    NSArray *found = [re matchesInString:body options:0 range:NSMakeRange(0, body.length)];
    // Every declaration must parse (arrays such as clip distances do not, and must not be skipped silently).
    if (found.count != [body componentsSeparatedByString:@";"].count - 1) return nil;
    for (NSTextCheckingResult *m in found) {
        NSString *attr = [m rangeAtIndex:4].location != NSNotFound ? [body substringWithRange:[m rangeAtIndex:4]] : @"";
        [members addObject:@{@"type": [body substringWithRange:[m rangeAtIndex:1]], @"name": [body substringWithRange:[m rangeAtIndex:2]], @"attr": attr}];
    }
    return members;
}
// Components and base type of a scalar / vector type name ("float3" -> 3, "float").
static int zelda_fsc_vector(NSString *type, NSString **base) {
    NSString *b = zelda_fsc_group(@"^(float|half|int|uint|short|ushort)([234]?)$", type, 1);
    if (!b) return 0;
    NSString *n = zelda_fsc_group(@"^(float|half|int|uint|short|ushort)([234]?)$", type, 2);
    if (base) *base = b;
    return n.length ? n.intValue : 1;
}
static NSString *const zelda_fsc_swizzle[] = {@"", @".x", @".xy", @".xyz", @""};
// Expression of `type` from a float4 (float types) or uint4 (integer types) value.
static NSString *zelda_fsc_unpack(NSString *type, NSString *value_f, NSString *value_u) {
    NSString *base; int n = zelda_fsc_vector(type, &base);
    if (!n) return nil;
    if ([base isEqualToString:@"float"] || [base isEqualToString:@"half"]) return [NSString stringWithFormat:@"%@(%@%@)", type, value_f, zelda_fsc_swizzle[n]];
    NSString *raw = [base hasPrefix:@"u"] ? value_u : [NSString stringWithFormat:@"as_type<int4>(%@)", value_u];
    return [NSString stringWithFormat:@"%@(%@%@)", type, raw, zelda_fsc_swizzle[n]];
}
// The entry point `kind main0(...)`: return type, parameter list and body ranges.
typedef struct { NSRange signature, params, body; NSString *ret; } zfc_entry;
static int zelda_fsc_entry(NSString *source, NSString *kind, zfc_entry *e) {
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"(?m)^%@ (\\w+) main0\\(", kind] options:0 error:nil];
    NSArray *all = [re matchesInString:source options:0 range:NSMakeRange(0, source.length)];
    if (all.count != 1) return 0;
    NSTextCheckingResult *m = all[0];
    NSUInteger open = NSMaxRange(m.range) - 1, close = zelda_fsc_close(source, open);
    if (close == NSNotFound) return 0;
    NSRange brace = [source rangeOfString:@"{" options:0 range:NSMakeRange(close, source.length - close)];
    if (brace.location == NSNotFound) return 0;
    NSUInteger end = zelda_fsc_close(source, brace.location);
    if (end == NSNotFound) return 0;
    e->ret = [source substringWithRange:[m rangeAtIndex:1]];
    e->signature = NSMakeRange(m.range.location, brace.location - m.range.location);
    e->params = NSMakeRange(open + 1, close - open - 1);
    e->body = NSMakeRange(brace.location, end - brace.location + 1);
    return 1;
}
// A parameter "type name [[attr]]" as {decl, name, attr}.
static NSDictionary *zelda_fsc_param(NSString *p) {
    NSString *attr = zelda_fsc_group(@"\\[\\[([^\\]]*)\\]\\]\\s*$", p, 1) ?: @"";
    NSString *decl = [[p stringByReplacingOccurrencesOfString:@"\\[\\[[^\\]]*\\]\\]\\s*$" withString:@"" options:NSRegularExpressionSearch range:NSMakeRange(0, p.length)]
                      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString *name = zelda_fsc_group(@"(\\w+)\\s*$", decl, 1);
    return name ? @{@"decl": decl, @"name": name, @"attr": attr} : nil;
}
// Declared type of a texture named `name` (descriptor-set member, array element or parameter), or nil.
static NSString *zelda_fsc_texture_type(NSString *source, NSString *name) {
    NSString *pattern = [NSString stringWithFormat:@"(?m)(?:array<)?(texture\\w+|depth\\w+)<[^\\n;]*?>\\s*(?:,\\s*\\d+>)?\\s+%@\\b", name];
    return zelda_fsc_group(pattern, source, 1);
}
// Argument-buffer index ([[id(n)]]) of a descriptor-set member, or -1.
static int zelda_fsc_aid(NSString *source, NSString *name) {
    NSString *id_ = zelda_fsc_group([NSString stringWithFormat:@"\\b%@\\s*\\[\\[id\\((\\d+)\\)\\]\\]", name], source, 1);
    return id_ ? id_.intValue : -1;
}
// Rewrites implicit-LOD samples in `body` to gradient samples from quad derivatives. Returns nil for samples a
// kernel cannot express (bias or LOD clamps on an implicit LOD). `implicit` gets [texture id, sampler id, implicit] of
// every sample: compute sampling picks the LOD of off-axis gradients differently from fragment sampling, so an implicit
// LOD must not matter (zelda_fsc_lod_free).
static NSString *zelda_fsc_gradients(NSString *body, NSString *source, NSMutableArray *implicit) {
    NSMutableString *out = [NSMutableString string];
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"(\\w+)(\\[[^\\]]*\\])?\\.(sample|sample_compare)\\(" options:0 error:nil];
    NSUInteger at = 0;
    for (NSTextCheckingResult *m in [re matchesInString:body options:0 range:NSMakeRange(0, body.length)]) {
        if (m.range.location < at) return nil;
        NSUInteger open = NSMaxRange(m.range) - 1, close = zelda_fsc_close(body, open);
        if (close == NSNotFound) return nil;
        NSArray<NSString *> *args = zelda_fsc_split([body substringWithRange:NSMakeRange(open + 1, close - open - 1)], 0);
        int explicit = 0;
        for (NSString *a in args) {
            if ([a hasPrefix:@"bias("] || [a hasPrefix:@"min_lod_clamp("]) return nil;
            if ([a hasPrefix:@"level("] || [a hasPrefix:@"gradient"]) explicit = 1;
        }
        // Every sample's texture / sampler is checked at the draw (interpolated coordinates are not bit-identical).
        NSString *sampler = args.count ? zelda_fsc_group(@"(\\w+)\\s*$", args[0], 1) : nil;
        int texture_aid = [body characterAtIndex:[m rangeAtIndex:1].location + [m rangeAtIndex:1].length] == '[' ? -1 :
                          zelda_fsc_aid(source, [body substringWithRange:[m rangeAtIndex:1]]);
        int sampler_aid = sampler ? zelda_fsc_aid(source, sampler) : -1;
        if (texture_aid < 0 || sampler_aid < 0) return nil;
        [implicit addObject:@[@(texture_aid), @(sampler_aid), @(!explicit)]];
        [out appendString:[body substringWithRange:NSMakeRange(at, open + 1 - at)]];
        at = open + 1;
        if (explicit) continue;
        NSString *type = zelda_fsc_texture_type(source, [body substringWithRange:[m rangeAtIndex:1]]);
        if (!type || args.count < 2) return nil;
        BOOL compare = [[body substringWithRange:[m rangeAtIndex:3]] isEqualToString:@"sample_compare"];
        NSString *gradient = [type hasPrefix:@"texture3d"] ? @"gradient3d" : [type containsString:@"cube"] ? @"gradientcube" :
                             [type hasPrefix:@"texture1d"] ? nil : @"gradient2d";
        if (!gradient) continue;   // 1D textures have no mip levels
        NSUInteger after = 1 + ([type hasSuffix:@"_array"] ? 1 : 0) + (compare ? 1 : 0);
        if (args.count <= after) return nil;
        NSMutableArray *rewritten = [args mutableCopy];
        NSString *c = args[1];
        [rewritten insertObject:[NSString stringWithFormat:@"%@(zfc_dx(%@, zql), zfc_dy(%@, zql))", gradient, c, c] atIndex:after + 1];
        [out appendString:[rewritten componentsJoinedByString:@", "]];
        at = close;
    }
    [out appendString:[body substringFromIndex:at]];
    return out;
}
// Interpolation of a fragment input from its attribute list: 0 perspective, 1 no_perspective, 2 flat; -1 unsupported.
static int zelda_fsc_interpolation(NSString *attr) {
    if ([attr containsString:@"flat"]) return 2;
    if ([attr containsString:@"sample_"]) return -1;
    return [attr containsString:@"no_perspective"] ? 1 : 0;
}

// Compute kernel `zfc_fragment` from a fragment shader. `colors` selects the outputs written (bit k = [[color(k)]]),
// each to texture(k); `implicit` gets the texture / sampler argument ids of implicit-LOD samples. Returns nil when the
// shader uses anything the kernel cannot reproduce.
static NSString *zelda_fsc_fragment_kernel(NSString *source, unsigned colors, NSMutableArray *implicit) {
    zfc_entry e;
    if (!source || !zelda_fsc_entry(source, @"fragment", &e) || ![e.ret isEqualToString:@"main0_out"]) return nil;
    NSString *body = [source substringWithRange:e.body];
    for (NSString *bad in @[@"simd_", @"quad_", @"atomic_", @".write(", @"dfdx_", @"dfdy_"])
        if ([body containsString:bad]) return nil;
    NSArray<NSDictionary *> *outs = zelda_fsc_members(source, @"main0_out");
    NSArray<NSDictionary *> *ins = zelda_fsc_members(source, @"main0_in") ?: @[];
    if (!outs) return nil;
    NSMutableString *params = [NSMutableString string], *args = [NSMutableString string], *kernel_params = [NSMutableString string];
    NSString *in_name = nil, *coord = nil, *facing = nil;
    for (NSString *p in zelda_fsc_split([source substringWithRange:e.params], 1)) {
        NSDictionary *q = zelda_fsc_param(p);
        if (!q) return nil;
        NSString *attr = q[@"attr"];
        if ([attr isEqualToString:@"stage_in"]) in_name = q[@"name"];
        else if ([attr isEqualToString:@"position"]) coord = q[@"name"];
        else if ([attr isEqualToString:@"front_facing"]) facing = q[@"name"];
        else if ([attr hasPrefix:@"buffer("]) [kernel_params appendFormat:@"%@, ", p];
        else return nil;
        [params appendFormat:@"%@, ", q[@"decl"]]; [args appendFormat:@"%@, ", q[@"name"]];
    }
    NSString *rewritten = zelda_fsc_gradients(body, source, implicit);
    if (!rewritten) return nil;
    NSMutableString *k = [NSMutableString string];
    [k appendString:[source substringToIndex:e.signature.location]];
    [k appendString:zelda_fsc_common];
    [k appendString:@"#define dfdx(v) zfc_dx(v, zql)\n#define dfdy(v) zfc_dy(v, zql)\n#define fwidth(v) zfc_fw(v, zql)\n#define discard_fragment() (zkill = true)\n"];
    [k appendFormat:@"static inline main0_out zfc_fs(%@ushort zql, thread bool &zkill)\n%@\n", params, rewritten];
    [k appendString:@"#undef dfdx\n#undef dfdy\n#undef fwidth\n#undef discard_fragment\n"];
    [k appendString:[source substringFromIndex:NSMaxRange(e.body)]];
    [k appendFormat:@"\nkernel void zfc_fragment(%@constant ZfcParams &zp [[buffer(%d)]], const device ZfcVert *zv [[buffer(%d)]]", kernel_params, ZFC_SLOT_PARAMS, ZFC_SLOT_VERTICES];
    NSMutableString *writes = [NSMutableString string];
    for (unsigned c = 0; c < 8; c++) if (colors >> c & 1) {
        NSDictionary *member = nil;
        for (NSDictionary *o in outs) if ([o[@"attr"] isEqualToString:[NSString stringWithFormat:@"color(%u)", c]]) member = o;
        NSString *base;
        if (!member || !zelda_fsc_vector(member[@"type"], &base) || !([base isEqualToString:@"float"] || [base isEqualToString:@"half"])) return nil;
        [k appendFormat:@", texture2d<float, access::write> zo%u [[texture(%u)]]", c, c];
        [writes appendFormat:@"    zo%u.write(zfc_out(out.%@), pix);\n", c, member[@"name"]];
    }
    for (NSDictionary *o in outs) if (![o[@"attr"] hasPrefix:@"color("]) return nil;
    [k appendString:@", ushort zt [[thread_index_in_threadgroup]], uint2 zg [[threadgroup_position_in_grid]])\n{\n"];
    [k appendString:@"    ushort zq = zt >> 2, zql = zt & 3;\n"
                    @"    uint2 pix = uint2(zp.org[0], zp.org[1]) + zg * 8 + uint2((zq & 3) * 2 + (zql & 1), (zq >> 2) * 2 + (zql >> 1));\n"
                    @"    float2 zpos = float2(pix) + 0.5f;\n    bool zdone = false, zpassed = false;\n"
                    // Like hardware, a quad is shaded once per triangle that covers one of its pixels, every lane
                    // extrapolating that triangle, so derivatives never mix two triangles.
                    @"    for (uint zti = 0; zti < zp.ntri; zti++)\n    {\n    ZfcHit zh = zfc_triangle(zp, zv, zpos, zti);\n"
                    @"    bool zmine = zh.covered && !zdone;\n    zdone = zdone || zh.covered;\n    if (!quad_any(zmine)) continue;\n"];
    if (in_name) {
        [k appendFormat:@"    main0_in %@ = {};\n", in_name];
        for (NSDictionary *m in ins) {
            NSString *loc = zelda_fsc_group(@"user\\(locn(\\d+)\\)", m[@"attr"], 1);
            int interp = zelda_fsc_interpolation(m[@"attr"]), L = loc.intValue;
            if (!loc || interp < 0 || L >= ZFC_LOCATIONS) return nil;
            NSString *base; if (!zelda_fsc_vector(m[@"type"], &base)) return nil;
            BOOL integer = !([base isEqualToString:@"float"] || [base isEqualToString:@"half"]);
            if (integer && interp != 2) return nil;
            NSString *f = [NSString stringWithFormat:@"%@(zv, zp, zh, %d)", interp == 1 ? @"zfc_linear" : @"zfc_perspective", L];
            NSString *u = [NSString stringWithFormat:@"zfc_flat(zv, zp, zh, %d)", L];
            if (interp == 2) f = [NSString stringWithFormat:@"as_type<float4>(%@)", u];
            [k appendFormat:@"    %@.%@ = %@;\n", in_name, m[@"name"], zelda_fsc_unpack(m[@"type"], f, u)];
        }
    }
    if (coord) [k appendFormat:@"    float4 %@ = float4(zpos, zh.z, zh.iw);\n", coord];
    if (facing) [k appendFormat:@"    bool %@ = zh.front;\n", facing];
    [k appendFormat:@"    bool zkill = false;\n    main0_out out = zfc_fs(%@zql, zkill);\n", args];
    [k appendString:@"    if (zmine && !zkill && pix.x >= zp.lo[0] && pix.y >= zp.lo[1] && pix.x < zp.hi[0] && pix.y < zp.hi[1])\n    {\n    zpassed = true;\n"];
    [k appendString:writes];
    [k appendString:@"    }\n    }\n"];
    // Samples passed (no depth or stencil test here: every written pixel), one atomic per SIMD group.
    [k appendString:@"    if (zp.samples)\n    {\n    uint zn = simd_sum(zpassed ? 1u : 0u);\n"
                    @"    if (simd_is_first() && zn) atomic_fetch_add_explicit(zp.samples, zn, memory_order_relaxed);\n    }\n}\n"];
    return k;
}

// Compute kernel `zfc_vertex` from a vertex shader: one thread per vertex of the draw.
static NSString *zelda_fsc_vertex_kernel(NSString *source) {
    zfc_entry e;
    if (!source || !zelda_fsc_entry(source, @"vertex", &e) || ![e.ret isEqualToString:@"main0_out"]) return nil;
    NSArray<NSDictionary *> *outs = zelda_fsc_members(source, @"main0_out");
    NSArray<NSDictionary *> *ins = zelda_fsc_members(source, @"main0_in") ?: @[];
    if (!outs) return nil;
    NSMutableString *params = [NSMutableString string], *args = [NSMutableString string], *kernel_params = [NSMutableString string], *locals = [NSMutableString string];
    for (NSString *p in zelda_fsc_split([source substringWithRange:e.params], 1)) {
        NSDictionary *q = zelda_fsc_param(p);
        if (!q) return nil;
        NSString *attr = q[@"attr"], *name = q[@"name"];
        if ([attr isEqualToString:@"stage_in"]) {
            [locals appendFormat:@"    main0_in %@ = {};\n", name];
            for (NSDictionary *m in ins) {
                NSString *a = zelda_fsc_group(@"^attribute\\((\\d+)\\)$", m[@"attr"], 1), *base;
                if (!a || a.intValue >= 16 || !zelda_fsc_vector(m[@"type"], &base)) return nil;
                if (!([base isEqualToString:@"float"] || [base isEqualToString:@"half"])) return nil;
                [locals appendFormat:@"    %@.%@ = %@;\n", name, m[@"name"], zelda_fsc_unpack(m[@"type"], [NSString stringWithFormat:@"zfc_fetch(zf.a[%@], zvid)", a], nil)];
            }
        } else if ([attr isEqualToString:@"vertex_id"]) [locals appendFormat:@"    %@ = zvid;\n", q[@"decl"]];
        else if ([attr isEqualToString:@"instance_id"] || [attr isEqualToString:@"base_instance"]) [locals appendFormat:@"    %@ = zf.base_instance;\n", q[@"decl"]];
        else if ([attr isEqualToString:@"base_vertex"]) [locals appendFormat:@"    %@ = 0;\n", q[@"decl"]];
        else if ([attr hasPrefix:@"buffer("]) [kernel_params appendFormat:@"%@, ", p];
        else return nil;
        [params appendFormat:@"%@%@", params.length ? @", " : @"", q[@"decl"]]; [args appendFormat:@"%@%@", args.length ? @", " : @"", name];
    }
    NSMutableString *stores = [NSMutableString string];
    NSString *position = nil;
    for (NSDictionary *o in outs) {
        NSString *attr = o[@"attr"], *loc = zelda_fsc_group(@"^user\\(locn(\\d+)\\)$", attr, 1);
        if ([attr hasPrefix:@"position"]) position = o[@"name"];
        else if (loc && loc.intValue < ZFC_LOCATIONS && zelda_fsc_vector(o[@"type"], NULL)) [stores appendFormat:@"    zv[zi].loc[%@] = zfc_pack(o.%@);\n", loc, o[@"name"]];
        else if (![attr isEqualToString:@"point_size"]) return nil;
    }
    if (!position) return nil;
    NSMutableString *k = [NSMutableString string];
    [k appendString:[source substringToIndex:e.signature.location]];
    [k appendString:zelda_fsc_common];
    [k appendFormat:@"static inline main0_out zfc_vs(%@)\n", params];
    [k appendString:[source substringWithRange:e.body]];
    [k appendString:[source substringFromIndex:NSMaxRange(e.body)]];
    [k appendFormat:@"\nkernel void zfc_vertex(%@constant ZfcFetch &zf [[buffer(%d)]], device ZfcVert *zv [[buffer(%d)]], uint zi [[thread_position_in_grid]])\n{\n",
                    kernel_params, ZFC_SLOT_FETCH, ZFC_SLOT_VERTICES];
    // A samples-passed slot starts at zero; the fragment kernel adds to its low word.
    [k appendString:@"    if (zi == 0 && zf.samples) *zf.samples = 0;\n    if (zi >= zf.count) return;\n    uint zvid = zf.vid[zi];\n"];
    [k appendString:locals];
    [k appendFormat:@"    main0_out o = zfc_vs(%@);\n    for (uint L = 0; L < %d; L++) zv[zi].loc[L] = uint4(0);\n    zv[zi].pos = o.%@;\n", args, ZFC_LOCATIONS, position];
    // Clipping is approximated per pixel; count the vertices that would need it so a run can tell it happened.
    [k appendFormat:@"    if (zf.clipped && (o.%@.w <= 0 || (zf.clip && (o.%@.z < 0 || o.%@.z > o.%@.w)))) atomic_fetch_add_explicit(zf.clipped, 1u, memory_order_relaxed);\n",
                    position, position, position, position];
    [k appendString:stores];
    [k appendString:@"}\n"];
    return k;
}

// Pixels a draw may write: scissor and target, and the viewport (primitives are clipped to |x|, |y| <= w, so no pixel
// centre outside the viewport is covered). org is the even-aligned dispatch origin (hardware quads start at even
// coordinates). Returns 0 when nothing is left.
static int zelda_fsc_bounds(const float vp[4], const long scissor[4], long width, long height, uint32_t org[2], uint32_t lo[2], uint32_t hi[2]) {
    long x0 = scissor[0] < 0 ? 0 : scissor[0], y0 = scissor[1] < 0 ? 0 : scissor[1];
    long x1 = MIN(scissor[0] + scissor[2], width), y1 = MIN(scissor[1] + scissor[3], height);
    double vx0 = MIN(vp[0], vp[0] + vp[2]), vx1 = MAX(vp[0], vp[0] + vp[2]), vy0 = MIN(vp[1], vp[1] + vp[3]), vy1 = MAX(vp[1], vp[1] + vp[3]);
    x0 = MAX(x0, (long)ceil(vx0 - 0.5)); x1 = MIN(x1, (long)floor(vx1 - 0.5) + 1);
    y0 = MAX(y0, (long)ceil(vy0 - 0.5)); y1 = MIN(y1, (long)floor(vy1 - 0.5) + 1);
    if (x1 <= x0 || y1 <= y0) return 0;
    lo[0] = (uint32_t)x0; lo[1] = (uint32_t)y0; hi[0] = (uint32_t)x1; hi[1] = (uint32_t)y1;
    org[0] = lo[0] & ~1u; org[1] = lo[1] & ~1u;
    return 1;
}

typedef struct { uint64_t ptr; uint32_t stride, fmt; } zfc_attr_t;
typedef struct { zfc_attr_t a[16]; uint32_t vid[8], count, base_instance, clip, pad; uint64_t clipped, samples; } zfc_fetch_t;
typedef struct { float vp[4], dr[2]; uint32_t org[2], lo[2], hi[2], ntri, cull, ccw, clip, tri[6]; uint64_t samples; } zfc_params_t;
enum { ZFC_VERTEX_BYTES = 16 + 16 * ZFC_LOCATIONS, ZFC_MAX_PIXELS = 800 * 448 };

// Colour formats a kernel writes with the same conversion as a render target: channel count, 0 if not handled.
static unsigned zelda_fsc_channels(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm:
        case MTLPixelFormatR16Float: case MTLPixelFormatR32Float: return 1;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm:
        case MTLPixelFormatRG16Float: case MTLPixelFormatRG32Float: return 2;
        case MTLPixelFormatRG11B10Float: return 3;
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
        case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA32Float: return 4;
        default: return 0;
    }
}
// Bytes of a vertex format the setup kernel decodes, 0 if not handled.
static unsigned zelda_fsc_vertex_size(MTLVertexFormat f) {
    switch ((unsigned)f) {
        case 45: case 46: case 47: case 48: return 1;
        case 1: case 4: case 7: case 10: case 49: case 50: case 51: case 52: case 53: return 2;
        case 2: case 5: case 8: case 11: return 3;
        case 3: case 6: case 9: case 12: case 13: case 16: case 19: case 22: case 25: case 28: case 32: case 36: case 40: case 41: case 42: return 4;
        case 14: case 17: case 20: case 23: case 26: return 6;
        case 15: case 18: case 21: case 24: case 27: case 29: case 33: case 37: return 8;
        case 30: case 34: case 38: return 12;
        case 31: case 35: case 39: return 16;
        default: return 0;
    }
}

#ifndef ZELDA_FSC_ANALYSIS_ONLY
// ---------- Part 2: draws run as kernels ----------
@interface ZfcKernel : NSObject { @public id<MTLComputePipelineState> vertex, fragment; NSArray *implicit; _Atomic int state; int logged; } @end
@implementation ZfcKernel @end
// Verification of one kernel on one target shape: it replaces the draw only after ZFC_VERIFY_PASSES checked passes
// (the draw renders, the kernel writes a copy, the two are compared) with no differing texel, and is dropped for good
// at the first difference; enabled kernels are re-checked every ZFC_RECHECK passes.
@interface ZfcVerify : NSObject { @public unsigned checked, clean; int failed, enabled; uint64_t uses; char name[96]; } @end
@implementation ZfcVerify @end
enum { ZFC_VERIFY_PASSES = 30, ZFC_RECHECK = 512 };
static NSMutableDictionary<NSString *, ZfcVerify *> *z_fsc_verify;
static unsigned z_fsc_verified, z_fsc_rejected;
static id<MTL4ArgumentTable> z_fsc_tables[2];   // vertex kernel, fragment kernel
static NSMutableDictionary<NSString *, ZfcKernel *> *z_fsc_kernels;
static dispatch_queue_t z_fsc_queue;
static id<MTLBuffer> z_fsc_flags;   // [0]: setup kernels that saw a vertex needing clipping
static uint64_t z_fsc_draws, z_fsc_checks, z_fsc_clears, z_fsc_skip[16], z_fsc_state[12], z_fsc_log_frame;
static unsigned z_fsc_ready, z_fsc_failed;
enum { ZFC_SKIP_STATE, ZFC_SKIP_PRIMITIVE, ZFC_SKIP_TARGET, ZFC_SKIP_SIZE, ZFC_SKIP_NEXT, ZFC_SKIP_ATTRIBUTE,
       ZFC_SKIP_COMPILING, ZFC_SKIP_KERNEL, ZFC_SKIP_LOD, ZFC_SKIP_WRITES, ZFC_SKIP_BOUNDS, ZFC_SKIP_FEEDBACK, ZFC_SKIP_REJECTED, ZFC_SKIP_VERIFYING };

static int zelda_fsc_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *v = getenv("ZELDA_FS_COMPUTE"); enabled = v && atoi(v) > 0; }
    return enabled;
}
// Texture usage for targets a kernel may write (called by tex_desc): ShaderWrite costs lossless compression, so only
// small colour targets get it.
static MTLTextureUsage zelda_fsc_usage(MTLTextureDescriptor *d, int not_renderable) {
    if (!zelda_fsc_enabled() || not_renderable || d.sampleCount > 1 || d.width * d.height > ZFC_MAX_PIXELS) return 0;
    if (d.textureType != MTLTextureType2D && d.textureType != MTLTextureType2DArray && d.textureType != MTLTextureTypeCube) return 0;
    return zelda_fsc_channels(d.pixelFormat) ? MTLTextureUsageShaderWrite : 0;
}
// ZELDA_FS_COMPUTE_KEYS=<fragment key prefix>[,...] limits conversion to those shaders (empty: none, targets still get
// ShaderWrite), for isolating a shader or the usage change.
static int zelda_fsc_selected(const char *key) {
    static NSArray<NSString *> *keys; static int init;
    if (!init) { init = 1; const char *v = getenv("ZELDA_FS_COMPUTE_KEYS"); if (v) keys = [@(v) componentsSeparatedByString:@","]; }
    if (!keys) return 1;
    for (NSString *k in keys) if (k.length && !strncmp(key, k.UTF8String, k.length)) return 1;
    return 0;
}
static void zelda_fsc_bind(int stage, uint64_t gpu, NSUInteger index) { [z_fsc_tables[stage ? 1 : 0] setAddress:gpu atIndex:index]; }

// Pending conditional colour clears (nvn_color_clear.h) land before the draw as a predicated fill, the same predicate
// as clear_if in nvn_query_msl.h.
static NSString *const zelda_fsc_clear_msl = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ZfcClear { float4 color; uint mode; uint known; uint x; uint y; uint right; uint bottom; };
kernel void zfc_clear(device const ulong *q [[buffer(0)]], constant ZfcClear &p [[buffer(1)]], texture2d<float, access::write> t [[texture(0)]],
                      uint2 g [[thread_position_in_grid]]) {
    ulong a = (p.known & 1) ? 0 : q[0], b = (p.known & 2) ? 0 : q[2];
    uint2 pix = uint2(p.x, p.y) + g;
    if (((a == b) == (p.mode == 0)) && pix.x < p.right && pix.y < p.bottom) t.write(p.color, pix);
}
)MSL";
typedef struct { float color[4]; uint32_t mode, known, x, y, right, bottom; } zfc_clear_t;
static id<MTLComputePipelineState> zelda_fsc_clear_pipeline(void) {
    static id<MTLComputePipelineState> pipeline; static int failed;
    if (!pipeline && !failed) {
        NSError *error = nil; id<MTLFunction> f = [ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:zelda_fsc_clear_msl options:nil error:&error]) newFunctionWithName:@"zfc_clear"];
        pipeline = f ? ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:f error:&error]) : nil;
        if (!pipeline) { failed = 1; MLOG("ERROR: FSC clear kernel: %s", error.localizedDescription.UTF8String); }
    }
    return pipeline;
}
// A fresh samples-passed slot for a converted draw while a query counts (as a counted render pass takes one).
static uint64_t zelda_fsc_vis_slot(void) {
    if (!g_vis_on || !g_vis_counting) return 0;
    if (!g_vis || g_vis_n >= VIS_SLOTS) {
        if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
        g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0;
    }
    uint32_t slot = g_vis_n++;
    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, (size_t)g_vscap * sizeof *g_vs); }
    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), slot};
    g_vis_cur = -1;   // the next counted render pass takes its own slot
    return g_vis.gpuAddress + 8ull * slot;
}
// The vertex function a draw uses is the fragment-linked variant (zelda_vertex_function with no integer attributes).
static NSString *zelda_fsc_vertex_text(NvShader *vs, NvShader *fs) {
    NSString *identity = [NSString stringWithFormat:@"%@-%08x-%@", [@(vs->key) substringToIndex:16], 0u, [@(fs->key) substringToIndex:16]];
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]; char name[17];
    CC_SHA256(identity.UTF8String, (CC_LONG)strlen(identity.UTF8String), hash);
    for (unsigned i = 0; i < 8; i++) snprintf(name + 2 * i, 3, "%02x", hash[i]);
    NSString *path = [g_msl_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s_b.metallib", name]];
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
// Kernels for a shader pair and output set; nil until the background compile finishes (the draw renders meanwhile).
static ZfcKernel *zelda_fsc_kernel(NvShader *vs, NvShader *fs, unsigned colors, int *state_out) {
    *state_out = 0;
    if (!z_fsc_kernels) {
        z_fsc_kernels = [NSMutableDictionary dictionary];
        z_fsc_queue = dispatch_queue_create("zelda.fullscreen-compute", DISPATCH_QUEUE_SERIAL);
        MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 31; d.maxTextureBindCount = 8;
        for (int i = 0; i < 2; i++) if (!(z_fsc_tables[i] = [g_dev newArgumentTableWithDescriptor:d error:nil])) MDIE("Fullscreen compute argument table");
        z_fsc_flags = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; res_add(z_fsc_flags);
    }
    NSString *key = [NSString stringWithFormat:@"%s|%s|%x", vs->key, fs->key, colors];
    ZfcKernel *k = z_fsc_kernels[key];
    if (k) {
        int state = atomic_load_explicit(&k->state, memory_order_acquire);
        if (state && !k->logged) {
            k->logged = 1; if (state > 0) z_fsc_ready++; else z_fsc_failed++;
            if (state > 0 && z_fsc_ready <= 64) MLOG("FSC kernel vs=%s fs=%s colors=%#x", vs->key, fs->key, colors);
            if (state < 0 && z_fsc_failed <= 32) MLOG("FSC kernel unavailable vs=%s fs=%s colors=%#x", vs->key, fs->key, colors);
        }
        *state_out = state;
        return state > 0 ? k : nil;
    }
    z_fsc_kernels[key] = k = [ZfcKernel new];
    NSString *vertex_text = zelda_fsc_vertex_text(vs, fs), *fragment_text = zelda_fetch_shader_text(fs);
    // Converted draws have depth test and write off, so a depth output has no effect.
    if ([fragment_text rangeOfString:@"[[depth("].location != NSNotFound) fragment_text = zelda_depthless_source(fragment_text);
    NSUInteger vabi = vs->rf.abi, fabi = fs->rf.abi; NSUInteger vlen = vs->arg_len, flen = fs->arg_len; int vnb = vs->rf.nb, fnb = fs->rf.nb;
    dispatch_async(z_fsc_queue, ^{ @autoreleasepool {
        NSMutableArray *implicit = [NSMutableArray array];
        NSString *vsrc = zelda_fsc_vertex_kernel(vertex_text), *fsrc = zelda_fsc_fragment_kernel(fragment_text, colors, implicit);
        id<MTLComputePipelineState> vp = nil, fp = nil;
        if (vsrc && fsrc) {
            MTLCompileOptions *vo = nil, *fo = nil; NSError *error = nil;
            vsrc = zelda_shader_math(vsrc, &vo); fsrc = zelda_shader_math(fsrc, &fo);
            id<MTLFunction> vf = [ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:vsrc options:vo error:&error]) newFunctionWithName:@"zfc_vertex"];
            id<MTLFunction> ff = vf ? [ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:fsrc options:fo error:&error]) newFunctionWithName:@"zfc_fragment"] : nil;
            // The kernels read the draw's own argument buffers, so their layouts must be the shaders' layouts.
            if (vf && ff && (!vnb || [vf newArgumentEncoderWithBufferIndex:vabi].encodedLength == vlen) &&
                (!fnb || [ff newArgumentEncoderWithBufferIndex:fabi].encodedLength == flen)) {
                vp = ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:vf error:&error]);
                fp = vp ? ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:ff error:&error]) : nil;
            }
        }
        k->vertex = vp; k->fragment = fp; k->implicit = implicit;
        atomic_store_explicit(&k->state, vp && fp && fp.maxTotalThreadsPerThreadgroup >= 64 ? 1 : -1, memory_order_release);
    } });
    return nil;
}
// Whether an implicit LOD cannot matter: a single-level view (or no mip filtering), equal minification and
// magnification filters, no anisotropy. Sampling that is sensitive to the last bit of a coordinate (nearest filtering
// or gather at a texel boundary) is left to the per-shape verification.
static int zelda_fsc_lod_free(NvShader *fs, NSArray *implicit) {
    for (NSArray *pair in implicit) {
        int texture_aid = [pair[0] intValue], sampler_aid = [pair[1] intValue], lod = [pair[2] intValue];
        const nvn_rbind *tb = NULL, *sb = NULL;
        for (int i = 0; i < fs->rf.nb; i++) { if (fs->rf.b[i].aid == texture_aid) tb = &fs->rf.b[i]; if (fs->rf.b[i].aid == sampler_aid) sb = &fs->rf.b[i]; }
        if (!tb || !sb || tb->kind != NVN_RB_TEX || (sb->kind != NVN_RB_SAMP && sb->kind != NVN_RB_SAMPT) || tb->reg >= 128 || sb->reg >= 128) return 0;
        int tid = (int)(S.tex[1][tb->reg] & 0xfffff), sid = (int)((sb->kind == NVN_RB_SAMPT ? S.tex[1][sb->reg] : S.samp[1][sb->reg]) >> 20);
        pthread_mutex_lock(&g_rmu);
        void *view = NULL; uint64_t rid = tid ? zelda_sampled_texture(tid, tb->dtt, &view) : 0;
        NSUInteger levels = view ? ((__bridge id<MTLTexture>)view).mipmapLevelCount : 0;
        int known = sid > 0 && sid < g_nsreg && g_sreg_ok[sid];
        samp_par sp = known ? g_sreg[sid] : (samp_par){0};
        pthread_mutex_unlock(&g_rmu);
        if (!rid || !levels || !known) return 0;
        int min_linear = sp.minf == 1 || sp.minf == 3 || sp.minf == 5, mag_linear = sp.magf != 0;
        if (lod && ((levels > 1 && sp.minf > 1) || sp.aniso > 1 || min_linear != mag_linear)) return 0;
    }
    return 1;
}
// A single-level 2D view of the attachment's subresource for texture writes, kept with the texture.
static id<MTLTexture> zelda_fsc_view(id<MTLTexture> t, NSUInteger level, NSUInteger slice) {
    if (t.textureType == MTLTextureType2D && t.mipmapLevelCount == 1) return t;
    static char key;
    NSMutableDictionary *views = objc_getAssociatedObject(t, &key);
    if (!views) objc_setAssociatedObject(t, &key, views = [NSMutableDictionary dictionary], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSNumber *k = @((uint64_t)level << 32 | slice);
    id<MTLTexture> v = views[k];
    if (!v) {
        v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(level, 1) slices:NSMakeRange(slice, 1)];
        if (v) views[k] = v;
    }
    return v;
}
// 1 if another draw follows in this pass before anything that ends it (same walk as ds_scan), 2 at the end of a list.
static int zelda_fsc_scan(const uint8_t *p, int depth) {
    for (int guard = 0; guard < 100000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }
        switch (h.op) {
            case OP_END: return 2;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8) return 1;
                int r = zelda_fsc_scan((const uint8_t *)t, depth + 1); if (r != 2) return r; break; }
            case OP_SET_RT: case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            case OP_DISPATCH: case OP_CLEAR_COLOR: case OP_CLEAR_DS: return 0;
            case OP_DRAW: case OP_DRAW_IDX: return 1;
            default: break;
        }
        p += h.size;
    }
    return 1;
}
static int zelda_fsc_alone(void) {
    if (!g_exec_next || g_ret_n > 16) return 0;
    int r = zelda_fsc_scan(g_exec_next, 0);
    for (int k = g_ret_n - 1; r == 2 && k >= 0; k--) r = zelda_fsc_scan(g_ret[k], 0);
    for (int k = 0; r == 2 && k < g_nrest; k++) r = zelda_fsc_scan((const uint8_t *)g_rest[k], 0);
    return r != 1;
}
static void zelda_fsc_check_log(void);
static int zelda_fsc_check(void);
static void zelda_fsc_log(void) {
    uint64_t frame = atomic_load(&g_cur_frame);
    if (frame < z_fsc_log_frame + 1200) return;
    z_fsc_log_frame = frame;
    if (zelda_fsc_check()) zelda_fsc_check_log();
    unsigned clipped = z_fsc_flags ? ((volatile uint32_t *)z_fsc_flags.contents)[0] : 0;
    MLOG("FSC draws=%llu checks=%llu verified=%u rejected=%u clears=%llu kernels=%u failed=%u clipped=%u skip state=%llu prim=%llu target=%llu size=%llu next=%llu attr=%llu compiling=%llu kernel=%llu lod=%llu writes=%llu bounds=%llu feedback=%llu rejected=%llu verifying=%llu",
         z_fsc_draws, z_fsc_checks, z_fsc_verified, z_fsc_rejected, z_fsc_clears, z_fsc_ready, z_fsc_failed, clipped, z_fsc_skip[0], z_fsc_skip[1], z_fsc_skip[2], z_fsc_skip[3], z_fsc_skip[4],
         z_fsc_skip[5], z_fsc_skip[6], z_fsc_skip[7], z_fsc_skip[8], z_fsc_skip[9], z_fsc_skip[10], z_fsc_skip[11], z_fsc_skip[12], z_fsc_skip[13]);
    MLOG("FSC state defer=%llu fuse=%llu other=%llu cond_clear=%llu ds_clear=%llu color_clear=%llu no_rt=%llu depth=%llu raster=%llu ui=%llu",
         z_fsc_state[1], z_fsc_state[2], z_fsc_state[3], z_fsc_state[4], z_fsc_state[10], z_fsc_state[11], z_fsc_state[6],
         z_fsc_state[7], z_fsc_state[8], z_fsc_state[9]);
}
// ZELDA_FS_COMPUTE_CHECK=1: kernels write scratch copies of the targets while the draw still renders; right after its
// pass, a compare kernel counts differing texels per shader (logged with the FSC lines). Nothing reaches the screen.
static int zelda_fsc_check(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("ZELDA_FS_COMPUTE_CHECK"); on = v && atoi(v) > 0; if (on) MLOG("FSC check mode: kernels write scratch targets"); }
    return on;
}
static NSString *const zelda_fsc_compare_msl = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void zfc_compare(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::read> b [[texture(1)]],
                        device atomic_uint *r [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= a.get_width() || g.y >= a.get_height()) return;
    float4 x = a.read(g), y = b.read(g);
    float4 d = select(abs(x - y), float4(0), isnan(x) && isnan(y));
    d = select(d, float4(1e30), isnan(x) != isnan(y));
    float m = max(max(d.x, d.y), max(d.z, d.w));
    if (!(m > 0)) return;
    atomic_fetch_add_explicit(&r[0], 1u, memory_order_relaxed);
    if (m > 0.01f) atomic_fetch_add_explicit(&r[1], 1u, memory_order_relaxed);
    atomic_fetch_max_explicit(&r[2], as_type<uint>(m), memory_order_relaxed);
    atomic_fetch_min_explicit(&r[3], g.x, memory_order_relaxed); atomic_fetch_min_explicit(&r[4], g.y, memory_order_relaxed);
    atomic_fetch_max_explicit(&r[5], g.x, memory_order_relaxed); atomic_fetch_max_explicit(&r[6], g.y, memory_order_relaxed);
}
)MSL";
enum { ZFC_CHECK_RECORDS = 16384 };
typedef struct { uint64_t fence, pass, texels; uint32_t w, h; char key[24]; void *verify; } zfc_check_record;
static struct { int pending; uint64_t pass; char key[24]; unsigned colors; __strong id<MTLTexture> targets[8], scratch[8];
                __strong id<MTLBuffer> arena; size_t vertices; int count; zfc_params_t params; __unsafe_unretained ZfcVerify *verify; } z_fsc_pending;
static zfc_check_record z_fsc_records[ZFC_CHECK_RECORDS]; static uint32_t z_fsc_record_n, z_fsc_record_read;
static id<MTLBuffer> z_fsc_results; static id<MTLComputePipelineState> z_fsc_compare;
static NSMutableDictionary<NSString *, NSMutableArray<NSNumber *> *> *z_fsc_stats;
// Scratch copies for checks, one per format and size (a few dozen small textures; placing them in one reused heap
// left their contents undefined after a texture of another layout had used the memory).
static id<MTLTexture> zelda_fsc_scratch(id<MTLTexture> like, int slot) {
    static NSMutableDictionary<NSString *, id<MTLTexture>> *cache;
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%lu|%lu|%lu|%d", (unsigned long)like.pixelFormat, (unsigned long)like.width, (unsigned long)like.height, slot];
    id<MTLTexture> t = cache[key];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:like.pixelFormat width:like.width height:like.height mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (!(t = [g_dev newTextureWithDescriptor:d])) return nil;
    res_add(t);
    return cache[key] = t;
}
// ZELDA_FS_COMPUTE_DUMP=<fragment key prefix>[:n[:width]]: the n-th (default 300th) checked pass (of that width) of that shader writes the rendered target and the
// kernel's copy (4-byte formats) as raw files to work/logs/fsc/dump-{render,kernel}-<w>x<h>.bin.
static struct { __strong id<MTLBuffer> render, kernel, vertices; uint64_t fence; uint32_t w, h, seen; int state; zfc_params_t params; } z_fsc_dump;
static void zelda_fsc_dump_write(void) {
    if (z_fsc_dump.state != 1 || z_fsc_dump.fence > g_done) return;
    z_fsc_dump.state = 2;
    NSString *dir = @"work/logs/fsc";
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSUInteger bytes = (NSUInteger)z_fsc_dump.w * z_fsc_dump.h * 4;
    [[NSData dataWithBytes:z_fsc_dump.render.contents length:bytes] writeToFile:[NSString stringWithFormat:@"%@/dump-render-%ux%u.bin", dir, z_fsc_dump.w, z_fsc_dump.h] atomically:YES];
    [[NSData dataWithBytes:z_fsc_dump.kernel.contents length:bytes] writeToFile:[NSString stringWithFormat:@"%@/dump-kernel-%ux%u.bin", dir, z_fsc_dump.w, z_fsc_dump.h] atomically:YES];
    MLOG("FSC dump written %ux%u", z_fsc_dump.w, z_fsc_dump.h);
    const zfc_params_t *p = &z_fsc_dump.params;
    MLOG("FSC dump params vp=%g,%g,%g,%g dr=%g,%g lo=%u,%u hi=%u,%u org=%u,%u ntri=%u cull=%u ccw=%u clip=%u tri=%u,%u,%u,%u,%u,%u",
         p->vp[0], p->vp[1], p->vp[2], p->vp[3], p->dr[0], p->dr[1], p->lo[0], p->lo[1], p->hi[0], p->hi[1], p->org[0], p->org[1],
         p->ntri, p->cull, p->ccw, p->clip, p->tri[0], p->tri[1], p->tri[2], p->tri[3], p->tri[4], p->tri[5]);
    const float *v = z_fsc_dump.vertices.contents;
    for (int i = 0; i < 6; i++) MLOG("FSC dump vertex %d pos=%.9g,%.9g,%.9g,%.9g loc0=%.9g,%.9g,%.9g,%.9g", i, v[68 * i], v[68 * i + 1], v[68 * i + 2], v[68 * i + 3],
                                     v[68 * i + 4], v[68 * i + 5], v[68 * i + 6], v[68 * i + 7]);
}
// Results whose submission has completed go into the per-shader totals.
static void zelda_fsc_check_collect(void) {
    zelda_fsc_dump_write();
    if (!z_fsc_results) return;
    if (!z_fsc_stats) z_fsc_stats = [NSMutableDictionary dictionary];
    const uint32_t *r = (const uint32_t *)z_fsc_results.contents;
    while (z_fsc_record_read < z_fsc_record_n) {
        const zfc_check_record *c = &z_fsc_records[z_fsc_record_read % ZFC_CHECK_RECORDS];
        if (c->fence > g_done) break;
        const uint32_t *v = r + 8 * (z_fsc_record_read % ZFC_CHECK_RECORDS);
        NSString *key = @(c->key);
        NSMutableArray<NSNumber *> *t = z_fsc_stats[key];
        if (!t) z_fsc_stats[key] = t = [@[@0, @0, @0, @0, @0.0, (NSNumber *)(id)@"-"] mutableCopy];
        ZfcVerify *verify = (__bridge ZfcVerify *)c->verify;
        if (verify && !verify->failed) {
            if (v[0]) {
                verify->failed = 1; verify->enabled = 0; z_fsc_rejected++;
                MLOG("FSC rejected %s: %u of %llu texels differ after %u clean checks", verify->name, v[0], (unsigned long long)c->texels, verify->clean);
            } else if (++verify->clean >= ZFC_VERIFY_PASSES && !verify->enabled) {
                verify->enabled = 1; z_fsc_verified++;
                if (z_fsc_verified <= 64) MLOG("FSC verified %s", verify->name);
            }
        }
        float maxd; memcpy(&maxd, &v[2], 4);
        t[0] = @(t[0].unsignedLongLongValue + 1); t[1] = @(t[1].unsignedLongLongValue + c->texels);
        t[2] = @(t[2].unsignedLongLongValue + v[0]); t[3] = @(t[3].unsignedLongLongValue + v[1]);
        if (maxd > t[4].doubleValue) { t[4] = @(maxd); t[5] = (NSNumber *)(id)[NSString stringWithFormat:@"%ux%u box %u,%u-%u,%u", c->w, c->h, v[3], v[4], v[5], v[6]]; }
        z_fsc_record_read++;
    }
}
static void zelda_fsc_check_log(void) {
    zelda_fsc_check_collect();
    for (NSString *key in [z_fsc_stats.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSArray<NSNumber *> *t = z_fsc_stats[key];
        MLOG("FSC check %s passes=%llu texels=%llu differing=%llu over_0.01=%llu max=%g (%s)", key.UTF8String, t[0].unsignedLongLongValue,
             t[1].unsignedLongLongValue, t[2].unsignedLongLongValue, t[3].unsignedLongLongValue, t[4].doubleValue, [(NSString *)(id)t[5] UTF8String]);
    }
}
// Called when a render pass ends (rp_end): compares the checked draw's targets with the kernel's scratch copies.
static void zelda_fsc_after_pass(void) {
    if (!z_fsc_pending.pending) return;
    z_fsc_pending.pending = 0;
    if (g_stat_pass != z_fsc_pending.pass) {   // the draw did not open the pass that just ended: no result
        if (z_fsc_pending.verify) z_fsc_pending.verify->checked--;
        return;
    }
    if (!z_fsc_compare) {
        NSError *error = nil; id<MTLFunction> f = [ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:zelda_fsc_compare_msl options:nil error:&error]) newFunctionWithName:@"zfc_compare"];
        z_fsc_compare = f ? ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:f error:&error]) : nil;
        z_fsc_results = [g_dev newBufferWithLength:ZFC_CHECK_RECORDS * 32 options:MTLResourceStorageModeShared]; res_add(z_fsc_results);
        if (!z_fsc_compare) MDIE("FSC compare kernel: %s", error.localizedDescription.UTF8String);
    }
    id<MTL4ComputeCommandEncoder> e = blit();
    zelda_blit_barrier();
    for (int i = 0; i < 8; i++) if (z_fsc_pending.colors >> i & 1) {
        if (z_fsc_record_n - z_fsc_record_read >= ZFC_CHECK_RECORDS) { if (z_fsc_pending.verify) z_fsc_pending.verify->checked--; break; }
        uint32_t slot = z_fsc_record_n++ % ZFC_CHECK_RECORDS;
        uint32_t *init = (uint32_t *)((uint8_t *)z_fsc_results.contents + 32 * slot);
        memset(init, 0, 32); init[3] = init[4] = UINT32_MAX;
        zfc_check_record *c = &z_fsc_records[slot];
        id<MTLTexture> t = z_fsc_pending.targets[i];
        c->fence = g_fence + 1; c->pass = z_fsc_pending.pass; c->texels = (uint64_t)t.width * t.height; c->w = (uint32_t)t.width; c->h = (uint32_t)t.height;
        snprintf(c->key, sizeof c->key, "%.16s/c%d", z_fsc_pending.key, i);
        c->verify = (__bridge void *)z_fsc_pending.verify;
        [z_fsc_tables[1] setTexture:t.gpuResourceID atIndex:0];
        [z_fsc_tables[1] setTexture:z_fsc_pending.scratch[i].gpuResourceID atIndex:1];
        [z_fsc_tables[1] setAddress:z_fsc_results.gpuAddress + 32 * slot atIndex:0];
        [e setArgumentTable:z_fsc_tables[1]];
        [e setComputePipelineState:z_fsc_compare];
        [e dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        const char *dump = getenv("ZELDA_FS_COMPUTE_DUMP");
        size_t dump_len = dump ? strcspn(dump, ":") : 0; unsigned dump_at = dump && dump[dump_len] ? (unsigned)atoi(dump + dump_len + 1) : 300;
        const char *dump_w = dump && dump[dump_len] ? strchr(dump + dump_len + 1, ':') : NULL;
        if (dump && !z_fsc_dump.state && i == 0 && !strncmp(z_fsc_pending.key, dump, dump_len) && (!dump_w || (NSUInteger)atoi(dump_w + 1) == t.width) &&
            ++z_fsc_dump.seen == dump_at) {
            NSUInteger bytes = t.width * t.height * 4;
            z_fsc_dump.render = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(z_fsc_dump.render);
            z_fsc_dump.kernel = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(z_fsc_dump.kernel);
            enc_b_copy_texture_buffer(e, t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), z_fsc_dump.render, 0, t.width * 4, bytes);
            enc_b_copy_texture_buffer(e, z_fsc_pending.scratch[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), z_fsc_dump.kernel, 0, t.width * 4, bytes);
            z_fsc_dump.vertices = [g_dev newBufferWithLength:6 * ZFC_VERTEX_BYTES options:MTLResourceStorageModeShared]; res_add(z_fsc_dump.vertices);
            enc_b_copy_buffer(e, z_fsc_pending.arena, z_fsc_pending.vertices, z_fsc_dump.vertices, 0, (NSUInteger)z_fsc_pending.count * ZFC_VERTEX_BYTES);
            z_fsc_dump.params = z_fsc_pending.params;
            z_fsc_dump.fence = g_fence + 1; z_fsc_dump.w = (uint32_t)t.width; z_fsc_dump.h = (uint32_t)t.height; z_fsc_dump.state = 1;
        }
    }
    zelda_blit_barrier();
    args_dirty_all();
    for (int i = 0; i < 8; i++) { z_fsc_pending.targets[i] = nil; z_fsc_pending.scratch[i] = nil; }
    z_fsc_pending.arena = nil;
}
#define ZFC_SKIP(reason) do { z_fsc_skip[reason]++; return 0; } while (0)

// Runs a draw that would open a render pass of its own as the two kernels in the copy/compute encoder. Returns 1 when
// the draw is done, 0 to render it.
static int zelda_fsc_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    if (!zelda_fsc_enabled() || g_enc || !pr) return 0;
    zelda_fsc_log();
    zelda_fsc_check_collect();
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !fs || pr->sh[2] || pr->sh[3] || pr->sh[4] || vs->z_layered || vs->z_wide_line || fs->rf.writes_smask || !zelda_fsc_selected(fs->key)) ZFC_SKIP(ZFC_SKIP_PRIMITIVE);
    int n = dw ? dw->count : di->count, prim = dw ? dw->prim : di->prim, ntri; uint32_t tri[6] = {0, 1, 2, 3, 4, 5};
    int instances = dw ? dw->instances : di->instances, base_instance = dw ? dw->base_instance : di->base_instance;
    if (instances != 1 || base_instance < 0 || (dw && dw->first < 0)) ZFC_SKIP(ZFC_SKIP_PRIMITIVE);
    if (prim == NVN_DRAW_PRIMITIVE_TRIANGLES && (n == 3 || n == 6)) ntri = n / 3;
    else if (prim == NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP && n == 4) { ntri = 2; uint32_t s[6] = {0, 1, 2, 2, 1, 3}; memcpy(tri, s, sizeof s); }
    else if (prim == 7 && n == 4 && dw) { ntri = 2; uint32_t q[6] = {0, 1, 2, 0, 2, 3}; memcpy(tri, q, sizeof q); }
    else ZFC_SKIP(ZFC_SKIP_PRIMITIVE);
    // Vertex ids in primitive order; indexed draws read their (at most six) indices here.
    uint32_t vids[8] = {0}; int64_t max_vid = 0;
    if (di) {
        unsigned size = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; void *m; uint64_t o; const uint8_t *ib;
        if (di->itype > 2 || pend_hits(di->ibuf, (uint64_t)n * size) || !mtl_addr(di->ibuf, (uint64_t)n * size, &m, &o) ||
            !(ib = [(__bridge id<MTLBuffer>)m contents])) ZFC_SKIP(ZFC_SKIP_PRIMITIVE);
        uint32_t restart = size == 1 ? S.restart_index & 0xff : size == 2 ? S.restart_index & 0xffff : S.restart_index;
        for (int k = 0; k < n; k++) {
            uint32_t v = size == 1 ? ib[o + k] : size == 2 ? ((const uint16_t *)(ib + o))[k] : ((const uint32_t *)(ib + o))[k];
            int64_t id = (int64_t)v + di->base_vertex;
            if ((S.restart && v == restart) || id < 0 || id > UINT32_MAX) ZFC_SKIP(ZFC_SKIP_PRIMITIVE);
            vids[k] = (uint32_t)id; max_vid = MAX(max_vid, id);
        }
    } else for (int k = 0; k < n; k++) { vids[k] = (uint32_t)(dw->first + k); max_vid = vids[k]; }
    // Targets the draw writes.
    unsigned colors = 0; __strong id<MTLTexture> views[8] = {nil}; long width = 0, height = 0;
    for (int i = 0; i < R.rt.n && i < 8; i++) {
        if (!R.rt.color[i]) continue;
        if (R.clear_c[i]) ZFC_SKIP(ZFC_SKIP_STATE);
        unsigned channels = S.chmask.mask >> (4 * i) & 15;
        if (!channels || !(fs->rf.color_mask >> i & 1)) continue;
        if ((S.color.blend_en >> i & 1) || fmt_compressed(R.rt.color[i]->d.format)) ZFC_SKIP(ZFC_SKIP_TARGET);
        MTLRenderPassColorAttachmentDescriptor *a = [MTLRenderPassColorAttachmentDescriptor new];
        attach(a, R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i], 1);
        id<MTLTexture> t = a.texture;
        unsigned count = t ? zelda_fsc_channels(t.pixelFormat) : 0, full = (1u << count) - 1;
        if (!t || !count || (channels & full) != full || t.sampleCount != 1 || t.textureType == MTLTextureType3D || !(t.usage & MTLTextureUsageShaderWrite)) ZFC_SKIP(ZFC_SKIP_TARGET);
        long w = (long)MAX(t.width >> a.level, 1u), h = (long)MAX(t.height >> a.level, 1u);
        if (colors && (w != width || h != height)) ZFC_SKIP(ZFC_SKIP_TARGET);
        for (int j = 0; j < i; j++) if ((colors >> j & 1) && zelda_same_subresource(i, j)) ZFC_SKIP(ZFC_SKIP_TARGET);
        if (!(views[i] = zelda_fsc_view(t, a.level, t.textureType == MTLTextureType2D ? 0 : a.slice))) ZFC_SKIP(ZFC_SKIP_TARGET);
        width = w; height = h; colors |= 1u << i;
    }
    if (!colors) ZFC_SKIP(ZFC_SKIP_TARGET);
    if (width * height > ZFC_MAX_PIXELS) ZFC_SKIP(ZFC_SKIP_SIZE);
    // A render pass samples a target's old contents; threads of a kernel would see their neighbours' new ones.
    if (draw_samples_rt_mask(pr, colors, 0)) ZFC_SKIP(ZFC_SKIP_FEEDBACK);
    if (!zelda_fsc_alone()) ZFC_SKIP(ZFC_SKIP_NEXT);
    {   // Machinery that restructures passes, pending work a pass would land, and state a kernel does not reproduce.
        unsigned clears = 0; for (int i = 0; i < 8; i++) clears |= (unsigned)R.clear_c[i] << i;
        int why = g_defer_replaying || g_defer.active ? 1 : g_fz.active || g_fz_checking ? 2 : g_frz_state || g_bloomq.state || g_rt_range_active || g_xv_n ? 3 :
                  z_depth_clear.pending || R.clear_d || R.clear_s ? 10 : clears ? 11 : !R.has_rt ? 6 :
                  S.ds.dt || S.ds.dw || S.ds.st ? 7 : S.ms.a2c || S.poly.mode != NVN_POLYGON_MODE_FILL ? 8 :
                  g_ui_rect_fn && n == 4 && S.nattr == 2 && S.stream[0].stride == 20 ? 9 : 0;
        if (why) { z_fsc_state[why]++; ZFC_SKIP(ZFC_SKIP_STATE); }
    }
    // Pending conditional clears on this target configuration: each target must take a kernel fill.
    __strong id<MTLTexture> clear_views[Z_COLOR_CLEAR_MAX] = {nil};
    for (int i = 0; i < z_color_clear.count; i++) {
        const zelda_color_clear_t *c = &z_color_clear.item[i];
        const tex_t *target = c->index < R.rt.n ? R.rt.color[c->index] : NULL;
        if (!target || fmt_compressed(target->d.format) || !zelda_fsc_clear_pipeline()) { z_fsc_state[4]++; ZFC_SKIP(ZFC_SKIP_STATE); }
        MTLRenderPassColorAttachmentDescriptor *a = [MTLRenderPassColorAttachmentDescriptor new];
        attach(a, target, &R.rt.cview[c->index], R.rt.has_cview[c->index], 1);
        id<MTLTexture> t = a.texture;
        if (!t || !zelda_fsc_channels(t.pixelFormat) || t.sampleCount != 1 || t.textureType == MTLTextureType3D || !(t.usage & MTLTextureUsageShaderWrite) ||
            !(clear_views[i] = zelda_fsc_view(t, a.level, t.textureType == MTLTextureType2D ? 0 : a.slice))) { z_fsc_state[4]++; ZFC_SKIP(ZFC_SKIP_STATE); }
    }
    // Vertex attributes, by the shader's attribute locations.
    zfc_fetch_t fetch = {0};
    memcpy(fetch.vid, vids, sizeof vids); fetch.count = (uint32_t)n; fetch.base_instance = (uint32_t)base_instance; fetch.clip = !S.depth_clamp;
    for (int i = 0; i < vs->rf.nv; i++) {
        unsigned attribute = vs->rf.v[i].nvn, loc = vs->rf.v[i].loc;
        if (attribute >= 16 || loc >= 16 || (int)attribute >= S.nattr) ZFC_SKIP(ZFC_SKIP_ATTRIBUTE);
        va_st at = S.attr[attribute];
        MTLVertexFormat format = at.stream < 16 ? vfmt(at.fmt) : MTLVertexFormatInvalid;
        unsigned size = zelda_fsc_vertex_size(format);
        if (!size || zelda_vertex_input_class(at.fmt)) ZFC_SKIP(ZFC_SKIP_ATTRIBUTE);
        vs_st stream = S.stream[at.stream]; NVNbufferRange buffer = S.vbuf[at.stream];
        uint64_t first = stream.divisor ? (uint64_t)base_instance / stream.divisor : 0;
        uint64_t last = stream.divisor ? first : (uint64_t)max_vid, end = last * stream.stride + at.offset + size;
        void *mb; uint64_t mo, gpu;
        if (!buffer.address || end > buffer.size || !mtl_gpu_addr(buffer.address, end, &mb, &mo, &gpu)) ZFC_SKIP(ZFC_SKIP_ATTRIBUTE);
        fetch.a[loc] = (zfc_attr_t){gpu + at.offset + (stream.divisor ? first * stream.stride : 0), stream.divisor ? 0 : stream.stride, (uint32_t)format};
    }
    zfc_params_t params = {{0, 0, (float)width, (float)height}, {0, 1}, {0, 0}, {0, 0}, {0, 0}, (uint32_t)ntri,
        S.poly.cull == NVN_FACE_FRONT ? 1u : S.poly.cull == NVN_FACE_BACK ? 2u : 0u, S.poly.front == NVN_FRONT_FACE_CCW, !S.depth_clamp, {0}, 0};
    memcpy(params.tri, tri, sizeof tri);
    if (S.has_vp) { memcpy(params.vp, S.vp[0], sizeof params.vp); params.dr[0] = S.dr[0]; params.dr[1] = S.dr[1]; }
    long scissor[4] = {0, 0, width, height};
    if (R.has_sc) { scissor[0] = R.sc.x; scissor[1] = R.sc.y; scissor[2] = R.sc.w; scissor[3] = R.sc.h; }
    if (!zelda_fsc_bounds(params.vp, scissor, width, height, params.org, params.lo, params.hi)) ZFC_SKIP(ZFC_SKIP_BOUNDS);
    int state; ZfcKernel *kernel = zelda_fsc_kernel(vs, fs, colors, &state);
    if (!kernel) ZFC_SKIP(state < 0 ? ZFC_SKIP_KERNEL : ZFC_SKIP_COMPILING);
    if (!zelda_fsc_lod_free(fs, kernel->implicit)) ZFC_SKIP(ZFC_SKIP_LOD);
    // Constant updates waiting for the next pass land first (copies in this same encoder).
    zelda_write_flush();
    if (zelda_write_pending()) ZFC_SKIP(ZFC_SKIP_WRITES);

    // Verification per kernel and target shape (sizes and formats of the written targets).
    if (!z_fsc_verify) z_fsc_verify = [NSMutableDictionary dictionary];
    NSMutableString *shape = [NSMutableString stringWithFormat:@"%.16s|%.16s|%x|%ldx%ld", vs->key, fs->key, colors, width, height];
    for (int i = 0; i < 8; i++) if (colors >> i & 1) [shape appendFormat:@"|%lu", (unsigned long)views[i].pixelFormat];
    ZfcVerify *verify = z_fsc_verify[shape];
    if (!verify) { z_fsc_verify[shape] = verify = [ZfcVerify new]; strlcpy(verify->name, shape.UTF8String, sizeof verify->name); }
    if (verify->failed) ZFC_SKIP(ZFC_SKIP_REJECTED);
    int check = zelda_fsc_check() || !verify->enabled || ++verify->uses % ZFC_RECHECK == 0;
    if (check && (z_fsc_pending.pending || verify->checked >= verify->clean + 8)) ZFC_SKIP(ZFC_SKIP_VERIFYING);   // one check per pass; results lag a few frames
    if (check) verify->checked++;
    __strong id<MTLTexture> scratch[8] = {nil};
    if (check) {
        // The kernels write copies of the targets (as they are before the draw); the draw itself renders.
        for (int i = 0; i < 8; i++) if (colors >> i & 1) if (!(scratch[i] = zelda_fsc_scratch(views[i], i))) { verify->checked--; ZFC_SKIP(ZFC_SKIP_VERIFYING); }
        BENC b = blit();
        for (int i = 0; i < 8; i++) if (colors >> i & 1)
            enc_b_copy_texture(b, views[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(views[i].width, views[i].height, 1), scratch[i], 0, 0, MTLOriginMake(0, 0, 0));
    }
    id<MTL4ComputeCommandEncoder> encoder = blit();
    zelda_blit_barrier();
    int clears = z_color_clear.count;
    for (int i = 0; i < clears; i++) {
        const zelda_color_clear_t *c = &z_color_clear.item[i];
        if (check) {   // only clears of written targets matter for the comparison, on the scratch copy
            if (!(colors >> c->index & 1)) continue;
            clear_views[i] = scratch[c->index];
        }
        zfc_clear_t p = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}, c->mode, c->known, (uint32_t)MAX(c->x, 0), (uint32_t)MAX(c->y, 0),
                         (uint32_t)MIN(c->right, (long)clear_views[i].width), (uint32_t)MIN(c->bottom, (long)clear_views[i].height)};
        if (p.right <= p.x || p.bottom <= p.y) continue;
        id<MTLBuffer> b; size_t o = arena_alloc(sizeof p, &b); memcpy((uint8_t *)b.contents + o, &p, sizeof p);
        [z_fsc_tables[1] setAddress:c->query atIndex:0];
        [z_fsc_tables[1] setAddress:b.gpuAddress + o atIndex:1];
        [z_fsc_tables[1] setTexture:clear_views[i].gpuResourceID atIndex:0];
        [encoder setArgumentTable:z_fsc_tables[1]];
        [encoder setComputePipelineState:zelda_fsc_clear_pipeline()];
        [encoder dispatchThreads:MTLSizeMake(p.right - p.x, p.bottom - p.y, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        if (!check) zskip_del(R.rt.color[c->index]->serial);
    }
    if (clears && !check) { z_color_clear.count = 0; z_color_clear_joined += (uint64_t)clears; z_fsc_clears += (uint64_t)clears; }
    if (clears) zelda_blit_barrier();
    uint64_t samples = check ? 0 : zelda_fsc_vis_slot();
    fetch.samples = samples; params.samples = samples;
    args_dirty_all();
    z_fsc_route = 1;
    encode_args((id)encoder, vs, 0, 1);
    encode_args((id)encoder, fs, 1, 0);
    z_fsc_route = 0;
    id<MTLBuffer> arena; size_t offset = arena_alloc(sizeof fetch + 16 + sizeof params + 16 + (size_t)n * ZFC_VERTEX_BYTES, &arena);
    uint8_t *cpu = (uint8_t *)arena.contents + offset; uint64_t gpu = arena.gpuAddress + offset;
    size_t point_at = (sizeof fetch + 15) & ~(size_t)15, params_at = point_at + 16, vertices_at = (params_at + sizeof params + 15) & ~(size_t)15;
    fetch.clipped = z_fsc_flags.gpuAddress;
    memcpy(cpu, &fetch, sizeof fetch); memcpy(cpu + point_at, &S.point_size, sizeof(float)); memcpy(cpu + params_at, &params, sizeof params);
    [z_fsc_tables[0] setAddress:gpu + point_at atIndex:15];
    [z_fsc_tables[0] setAddress:gpu atIndex:ZFC_SLOT_FETCH];
    [z_fsc_tables[0] setAddress:gpu + vertices_at atIndex:ZFC_SLOT_VERTICES];
    [encoder setArgumentTable:z_fsc_tables[0]];
    [encoder setComputePipelineState:kernel->vertex];
    [encoder dispatchThreads:MTLSizeMake((NSUInteger)n, 1, 1) threadsPerThreadgroup:MTLSizeMake((NSUInteger)n, 1, 1)];
    zelda_blit_barrier();
    [z_fsc_tables[1] setAddress:gpu + params_at atIndex:ZFC_SLOT_PARAMS];
    [z_fsc_tables[1] setAddress:gpu + vertices_at atIndex:ZFC_SLOT_VERTICES];
    for (int i = 0; i < 8; i++) if (colors >> i & 1) [z_fsc_tables[1] setTexture:(check ? scratch[i] : views[i]).gpuResourceID atIndex:(NSUInteger)i];
    [encoder setArgumentTable:z_fsc_tables[1]];
    [encoder setComputePipelineState:kernel->fragment];
    [encoder dispatchThreadgroups:MTLSizeMake((params.hi[0] - params.org[0] + 7) / 8, (params.hi[1] - params.org[1] + 7) / 8, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    zelda_blit_barrier();
    args_dirty_all();
    if (check) {   // render the draw; compare when its pass ends
        z_fsc_pending.pending = 1; z_fsc_pending.pass = g_stat_pass + 1; z_fsc_pending.colors = colors;
        snprintf(z_fsc_pending.key, sizeof z_fsc_pending.key, "%s", fs->key);
        for (int i = 0; i < 8; i++) { z_fsc_pending.targets[i] = views[i]; z_fsc_pending.scratch[i] = scratch[i]; }
        z_fsc_pending.arena = arena; z_fsc_pending.vertices = offset + vertices_at; z_fsc_pending.count = n; z_fsc_pending.params = params;
        z_fsc_pending.verify = verify;
        z_fsc_checks++;
        return 0;
    }
    // What a rendered draw records: its targets are no longer known zero, and buffer caches see the texture writes.
    for (int i = 0; i < 8; i++) if (colors >> i & 1) zskip_del(R.rt.color[i]->serial);
    memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok);
    zelda_buffer_targets_written();
    g_stat_draw++; z_fsc_draws++;
    PD("G %llu fullscreen compute fs=%s colors=%#x %ldx%ld", g_stat_pass, fs->key, colors, width, height);
    return 1;
}
#endif
