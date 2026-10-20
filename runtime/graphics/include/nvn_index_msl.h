#pragma once
static const char zelda_index_msl[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void expand_index(device const uchar *source [[buffer(0)]], device ushort *target [[buffer(1)]], "
    "constant uint &count [[buffer(2)]], uint i [[thread_position_in_grid]]) { if (i < count) target[i] = source[i]; }\n"
    // The same conversion drawn inside the render pass that uses the indices (one vertex per index, no rasterization).
    "vertex void expand_index_vertex(device const uchar *source [[buffer(0)]], device ushort *target [[buffer(1)]], "
    "uint i [[vertex_id]]) { target[i] = source[i]; }\n";
