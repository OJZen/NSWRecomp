static void zelda_compute_uniform_table(uint8_t *driver, const p_bind_buf buffers[16]) {
    for (unsigned r = 0; r < 16; r++) {
        uint64_t address = buffers[r].addr;
        uint32_t size = (uint32_t)buffers[r].size;
        memcpy(driver + 0x210 + 16 * r, &address, sizeof address);
        memcpy(driver + 0x218 + 16 * r, &size, sizeof size);
    }
}
