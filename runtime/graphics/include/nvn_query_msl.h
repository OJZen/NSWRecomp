static const char zelda_query_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void query_sum(device ulong *dst [[buffer(0)]], constant ulong *p [[buffer(1)]]) {\n"
" ulong sum = 0; for (ulong i = 0; i < p[0]; i++) sum += *((device const ulong *)p[i + 1]); dst[0] = sum; }\n"
"struct ClearVertex { float4 position [[position]]; };\n"
// known bit 0 / 1: the counter word at +0 / +16 is zero because a timestamp report to it is still pending.
"struct ClearParams { float4 color; uint mode; uint known; };\n"
"float2 clear_position(uint i, device const ulong *q, constant ClearParams &p) {\n"
" float2 xy[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
" ulong a = (p.known & 1) ? 0 : q[0], b = (p.known & 2) ? 0 : q[2];\n"
// A false predicate makes a degenerate triangle, including for depth/stencil-only passes.
" return ((a == b) == (p.mode == 0)) ? xy[i] : float2(0); }\n"
"vertex ClearVertex clear_vertex(uint i [[vertex_id]], device const ulong *q [[buffer(0)]], constant ClearParams &p [[buffer(1)]]) {\n"
" return {float4(clear_position(i,q,p),0,1)}; }\n"
"vertex ClearVertex depth_clear_vertex(uint i [[vertex_id]], device const ulong *q [[buffer(0)]], constant ClearParams &p [[buffer(1)]]) {\n"
" return {float4(clear_position(i,q,p),p.color.x,1)}; }\n"
"fragment float4 clear_if(constant ClearParams &p [[buffer(1)]]) { return p.color; }\n"
// Clears drawn inside a game pass write one colour slot of a multi-target pass.
"#define CLEAR_AT(k) struct ClearOut##k { float4 color [[color(k)]]; };\\\n"
" fragment ClearOut##k clear_if##k(constant ClearParams &p [[buffer(1)]]) { return {p.color}; }\n"
"CLEAR_AT(0) CLEAR_AT(1) CLEAR_AT(2) CLEAR_AT(3) CLEAR_AT(4) CLEAR_AT(5) CLEAR_AT(6) CLEAR_AT(7)\n"
// Constant update written at the start of a render pass: one 32-bit word per vertex, no rasterization.
"vertex void buffer_write(uint id [[vertex_id]], device uint *destination [[buffer(0)]], const device uint *source [[buffer(1)]]) {\n"
"    destination[id] = source[id];\n"
"}\n";
