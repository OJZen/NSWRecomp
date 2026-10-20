#pragma once
#include <stdint.h>

typedef struct {
    uint64_t predicate;
    uint32_t mode, indexed, args[5], padding;
} zelda_conditional_input;
typedef struct {
    uint32_t args[5], enabled;
    uint64_t left, right;
} zelda_conditional_output;
_Static_assert(sizeof(zelda_conditional_input) == 40 && sizeof(zelda_conditional_output) == 40, "Indirect parameter layout");

static const char zelda_conditional_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct ConditionalInput { device const ulong *predicate; uint mode, indexed, args[5], padding; };\n"
"struct ConditionalOutput { uint args[5], enabled; ulong left, right; };\n"
"kernel void conditional_draw(device const ConditionalInput *input [[buffer(0)]],\n"
"    device ConditionalOutput *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"    ConditionalInput p = input[i];\n"
"    ulong a = p.predicate[0], b = p.predicate[2];\n"
"    bool enabled = ((a == b) == (p.mode == 0));\n"
"    for (uint k = 0; k < 5; k++) output[i].args[k] = p.args[k];\n"
"    output[i].args[0] = enabled ? p.args[0] : 0;\n"
"    output[i].enabled = enabled; output[i].left = a; output[i].right = b;\n"
"}\n";
