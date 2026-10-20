static void h_nvnCommandBufferBindTexturesDeferred(NVNcommandBuffer *c, NVNshaderStage stage,
        int first, int count, const NVNtextureHandle *handles) {
    if ((unsigned)stage >= 6 || (unsigned)first > 128 || count < 0 || count > 128 - first ||
        (count && !handles) || ((uintptr_t)handles & 7) || (uintptr_t)handles > UINTPTR_MAX - (size_t)count * 8) {
        LOG("BLOCKED NVN deferred texture range stage=%d first=%d count=%d", stage, first, count); exit(78);
    }
    for (int offset = 0; offset < count; offset += 32) {
        p_bind_handles *record = RECN(c, OP_BIND_TEX, p_bind_handles, h, 0);
        record->stage = stage; record->first = first + offset;
        record->n = count - offset < 32 ? count - offset : 32;
        record->deferred = handles + offset;
    }
}
