// Synthetic SPIR-V 1.0: buffer[global_id.x] = buffer[global_id.x] * 3 + 7.
// One storage buffer (set 0, binding 0), local size 8. No game-derived bytes.
#pragma once
#include <stdint.h>
#define SPV(op,n) (((n)<<16)|(op))
static const uint32_t synthetic_compute_spirv[]={
    0x07230203,0x00010000,0,26,0,
    SPV(17,2),1,                         // OpCapability Shader
    SPV(14,3),0,1,                       // Logical GLSL450
    SPV(15,6),5,1,0x6e69616d,0,2,         // GLCompute main, interface global_id
    SPV(16,6),1,17,8,1,1,                // LocalSize
    SPV(71,4),2,11,28,                   // BuiltIn GlobalInvocationId
    SPV(71,4),8,6,4,                     // ArrayStride 4
    SPV(72,5),9,0,35,0,                  // Member Offset 0
    SPV(71,3),9,3,                       // BufferBlock
    SPV(71,4),11,34,0,                   // DescriptorSet 0
    SPV(71,4),11,33,0,                   // Binding 0
    SPV(19,2),3,                         // void
    SPV(33,3),4,3,                       // void function type
    SPV(21,4),5,32,0,                    // uint
    SPV(23,4),6,5,3,                     // uvec3
    SPV(32,4),7,1,6,                     // pointer Input uvec3
    SPV(59,4),7,2,1,                     // global_id
    SPV(29,3),8,5,                       // runtime uint array
    SPV(30,3),9,8,                       // block struct
    SPV(32,4),10,2,9,                    // pointer Uniform block
    SPV(59,4),10,11,2,                   // storage buffer (BufferBlock)
    SPV(32,4),12,2,5,                    // pointer Uniform uint
    SPV(43,4),5,13,0,                    // uint 0
    SPV(43,4),5,14,3,                    // uint 3
    SPV(43,4),5,15,7,                    // uint 7
    SPV(54,5),3,1,0,4,                   // function main
    SPV(248,2),16,                       // label
    SPV(61,4),6,17,2,                    // load global_id
    SPV(81,5),5,18,17,0,                 // extract x
    SPV(65,6),12,19,11,13,18,            // buffer[0][x]
    SPV(61,4),5,20,19,                   // load value
    SPV(132,5),5,21,20,14,               // multiply by 3
    SPV(128,5),5,22,21,15,               // add 7
    SPV(62,3),19,22,                     // store value
    SPV(253,1),                          // return
    SPV(56,1)                           // function end
};
#undef SPV
