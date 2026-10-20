NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t enable) {
    p_render_enable *r = REC(c, OP_RENDER_ENABLE, p_render_enable);
    r->enabled = !!(enable & 255);
}
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c,
        NVNconditionalRenderMode mode, NVNbufferAddress address) {
    if ((unsigned)mode > 1 || !address || (address & 15) || address > UINT64_MAX - 32) {
        LOG("BLOCKED NVN conditional render range"); exit(78);
    }
    p_render_enable *r = REC(c, OP_RENDER_ENABLE, p_render_enable);
    r->address = address; r->mode = mode;
}
