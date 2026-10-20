#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <string.h>

static uint32_t shader_word(const void *p, size_t offset) {
    uint32_t value; memcpy(&value, (const uint8_t *)p + offset, sizeof value); return value;
}

// Match index_shaders.stage: shared control blocks alone do not identify Zelda bytecode.
static int zelda_shader_hash(const void *control, const void *data, uint8_t digest[32]) {
    if (!control || !data || shader_word(control, 0) != 0x98761234 || shader_word(control, 4) != 1) return 0;
    uint32_t version = shader_word(control, 8), length = shader_word(control, 0x20);
    if ((version != 9 && version != 16) || length < 0x748 || length > 0x10000) return 0;
    uint32_t program = shader_word(control, 0x6f8), constants = shader_word(control, 0x6fc);
    uint32_t c1 = shader_word(control, 0x700), total = shader_word(control, 0x704);
    uint32_t offset = shader_word(control, 0x708), stage = shader_word(control, 0x714);
    if (stage > 5 || offset != (stage == 5 ? 0x100u : 0x30u) || total > (8u << 20) ||
        c1 < offset || program > c1 - offset || c1 > total || constants > total - c1 ||
        shader_word(data, 0) != (stage == 5 ? 0x12345679u : 0x12345678u)) return 0;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, control, length);
    CC_SHA256_Update(&context, (const uint8_t *)data + offset, c1 - offset);
    CC_SHA256_Update(&context, (const uint8_t *)data + c1, constants);
    CC_SHA256_Final(digest, &context);
    return 1;
}
