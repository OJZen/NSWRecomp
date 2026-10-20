// Identical driver constants share immutable arena storage until this command-buffer slice completes.
static int z_point_params_reuse = -1;
static uint64_t z_point_params_created, z_point_params_reused;
static uint64_t zelda_point_params(float point_size, uint32_t face) {
    static uint64_t submit, previous, gpu;
    uint32_t words[2] = {0, face}; memcpy(words, &point_size, sizeof point_size);
    uint64_t bits; memcpy(&bits, words, sizeof bits);
    if (z_point_params_reuse < 0) {
        const char *value = getenv("ZELDA_POINT_PARAMS_REUSE"); z_point_params_reuse = !value || atoi(value);
    }
    if (z_point_params_reuse && gpu && submit == g_sub_serial && bits == previous) {
        z_point_params_reused++; return gpu;
    }
    id<MTLBuffer> buffer; size_t offset = arena_alloc(sizeof words, &buffer);
    memcpy((uint8_t *)buffer.contents + offset, words, sizeof words);
    submit = g_sub_serial; previous = bits; gpu = buffer.gpuAddress + offset;
    z_point_params_created++; return gpu;
}
