#pragma once
// Binding kind for read-only storage buffers whose address a shader loads from a game uniform buffer
// (reflection kind "sbuf_cb"; nvn_pointer_storage.inc). reg is the NVN uniform slot; dtt | dpf << 8 the byte offset.
enum { ZELDA_RB_SBUF_CB = 0x40 };
