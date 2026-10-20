// Storage buffers addressed through a game uniform buffer. v1.4.3 cave_quadmesh vertex shaders read global memory at
// the 64-bit address stored in uniform slot 3 at byte 0 (Maxwell c6[0]); build_shaders.py reflects them as "sbuf_cb".
// The upstream parser knows only storage slots bound with BindStorageBuffer, so these are parsed as storage bindings
// and retagged; at each argument encode the address is read from the bound uniform buffer and translated like any other
// GPU address. Memory written by a GPU copy that has not run yet (this submission, or one still on the GPU) is read from
// the newest such update's payload; an unknown pending write over the address stops.
// Included twice: before shader_load (parsing) and, with ZELDA_POINTER_STORAGE_BIND, before encode_args (binding).
#include "nvn_pointer_storage.h"
#ifndef ZELDA_POINTER_STORAGE_BIND
static int zelda_refl_from_json(NSDictionary *json, nvn_refl *reflection, char *error, int length) {
    NSArray *bindings = json[@"bindings"];
    NSMutableArray *parsed = nil; NSMutableDictionary<NSNumber *, NSArray *> *pointers = nil;
    if ([bindings isKindOfClass:[NSArray class]])
        for (NSUInteger i = 0; i < bindings.count; i++) {
            NSDictionary *binding = bindings[i];
            if (![binding isKindOfClass:[NSDictionary class]] || ![binding[@"kind"] isEqual:@"sbuf_cb"]) continue;
            int slot = [binding[@"reg"] intValue], offset = [binding[@"offset"] intValue];
            if (![binding[@"reg"] isKindOfClass:[NSNumber class]] || slot < 0 || slot >= 16 || offset < 0 || offset % 8 ||
                offset + 8 > 0x10000 || [binding[@"rw"] boolValue]) {
                snprintf(error, length, "uniform-addressed storage binding %s is unsupported", [binding[@"msl_name"] UTF8String]);
                return 1;
            }
            if (!parsed) { parsed = [bindings mutableCopy]; pointers = [NSMutableDictionary dictionary]; }
            NSMutableDictionary *plain = [binding mutableCopy]; plain[@"kind"] = @"sbuf"; plain[@"reg"] = @0; plain[@"rw"] = @NO;
            parsed[i] = plain; pointers[@(i)] = @[@(slot), @(offset)];
        }
    NSDictionary *input = json;
    if (parsed) { NSMutableDictionary *copy = [json mutableCopy]; copy[@"bindings"] = parsed; input = copy; }
    if (nvn_refl_from_json(input, reflection, error, length)) return 1;
    for (NSNumber *index in pointers) {
        nvn_rbind *binding = &reflection->b[index.intValue]; int offset = [pointers[index][1] intValue];
        binding->kind = ZELDA_RB_SBUF_CB; binding->reg = (uint16_t)[pointers[index][0] intValue];
        binding->dtt = (uint8_t)offset; binding->dpf = (uint8_t)(offset >> 8);
    }
    return 0;
}

#else
static uint64_t z_pointer_storage_binds;
// GPU writes to memory, in stream order, kept until their submission completes. payload is the update's source bytes
// (command memory, valid until then); NULL for writes whose bytes are not known on the CPU.
enum { Z_UPDATE_LOG = 4096 };
static struct { uint64_t destination, length, fence; const uint8_t *payload; } z_update_log[Z_UPDATE_LOG];
static unsigned z_update_next, z_update_count;
static uint64_t z_update_lost;   // fence of the newest entry overwritten before completing
static void zelda_update_log_op(unsigned op, const void *data) {
    uint64_t destination, length; const uint8_t *payload = NULL;
    switch (op) {
        case OP_UPDATE_UB: memcpy(&destination, data, 8); memcpy(&length, (const uint8_t *)data + 8, 8); payload = (const uint8_t *)data + 16; break;
        case OP_COPY_B2B: memcpy(&destination, (const uint8_t *)data + 8, 8); memcpy(&length, (const uint8_t *)data + 16, 8); break;
        case OP_REPORT_COUNTER: memcpy(&destination, (const uint8_t *)data + 8, 8); length = 16; break;
        case OP_SIGNAL_EVENT: memcpy(&destination, data, 8); length = 4; break;
        default: return;
    }
    uint64_t canonical = nvn_canonical_address(destination, length);   // virtual pool aliases share one record
    if (canonical) destination = canonical;
    if (z_update_count == Z_UPDATE_LOG) {
        uint64_t fence = z_update_log[z_update_next].fence;
        if (!mtl_wait(fence, 0) && fence > z_update_lost) z_update_lost = fence;
    } else z_update_count++;
    // Commands of the open slice complete with the next committed fence.
    z_update_log[z_update_next] = (typeof(z_update_log[0])){destination, length, mtl_last_fence() + 1, payload};
    z_update_next = (z_update_next + 1) % Z_UPDATE_LOG;
}
// The 8 bytes at address as the GPU sees them at this point of the stream.
static uint64_t zelda_gpu_view_u64(uint64_t address, const uint8_t *cpu, const char *shader) {
    uint64_t canonical = nvn_canonical_address(address, 8), raw = address;
    if (canonical) address = canonical;
    if (z_update_lost && !mtl_wait(z_update_lost, 0)) mtl_wait(z_update_lost, UINT64_MAX);
    for (unsigned i = 1; i <= z_update_count; i++) {
        const typeof(z_update_log[0]) *e = &z_update_log[(z_update_next + Z_UPDATE_LOG - i) % Z_UPDATE_LOG];
        if (address + 8 <= e->destination || e->destination + e->length <= address) continue;
        if (mtl_wait(e->fence, 0)) break;   // older entries completed before this one
        if (!e->payload || address < e->destination || address + 8 > e->destination + e->length)
            MDIE("Storage address of shader %s is overwritten by a pending GPU write the CPU cannot read", shader);
        uint64_t value; memcpy(&value, e->payload + (address - e->destination), 8); return value;
    }
    if (pend_hits(raw, 8)) MDIE("Storage address of shader %s has an unlogged pending GPU write", shader);
    uint64_t value; memcpy(&value, cpu, 8); return value;
}
// Metal GPU address of the storage the shader reaches through uniform slot reg; 0 when the slot or address is unbound.
static uint64_t zelda_pointer_storage(int stage, const nvn_rbind *binding, const char *shader, void **buffer) {
    unsigned offset = binding->dtt | (unsigned)binding->dpf << 8;
    const p_bind_buf *uniform = binding->reg < 16 && stage >= 0 && stage < 6 ? &S.ubo[stage][binding->reg] : NULL;
    if (!uniform || !uniform->addr || offset + 8 > uniform->size) return 0;
    void *memory; uint64_t at, gpu;
    if (!mtl_addr(uniform->addr + offset, 8, &memory, &at)) return 0;
    const uint8_t *bytes = [(__bridge id<MTLBuffer>)memory contents];
    if (!bytes) MDIE("Uniform buffer holding a storage address is not CPU visible");
    uint64_t address = zelda_gpu_view_u64(uniform->addr + offset, bytes + at, shader);
    if (!address) return 0;
    if (!mtl_gpu_addr(address, 4, buffer, &at, &gpu)) {
        RED_ONCE(60, "Shader %s storage address from constant slot %u+%u is outside memory pools: %#llx", shader, binding->reg, offset, (unsigned long long)address);
        return 0;
    }
    if (!z_pointer_storage_binds++)
        MLOG("Constant-pointer storage: first shader=%s stage=%d constant_slot=%u+%u address=%#llx", shader, stage, binding->reg, offset, (unsigned long long)address);
    use_rec("buf", address, 4, 0, NULL);
    return gpu;
}
#endif
