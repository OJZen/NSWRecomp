// Buffers are bounded views of the shared, coherent Metal memory pool.
typedef struct { NVNdevice *device; pool_t *pool; ptrdiff_t offset; size_t size; } z_buffer;
FITS(z_buffer, NVNbufferBuilder); FITS(z_buffer, NVNbuffer);
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { ((z_buffer *)b)->device = d; }
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) {
    NVNdevice *device = ((z_buffer *)b)->device;
    *(z_buffer *)b = (z_buffer){.device = device};
}
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t size) {
    z_buffer *v = (z_buffer *)b; v->pool = (pool_t *)p; v->offset = off; v->size = size;
}
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *builder) {
    const z_buffer *v = (const z_buffer *)builder;
    if (!v->device || !v->pool || !v->pool->mem || v->offset < 0 ||
        (uint64_t)v->offset > v->pool->size || v->size > v->pool->size - (uint64_t)v->offset) return 0;
    *(z_buffer *)b = *v; return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *b) { memset(b, 0, sizeof(z_buffer)); }
NVN(nvnBufferMap, void *, const NVNbuffer *b) {
    const z_buffer *v = (const z_buffer *)b; return v->pool->mem + v->offset;
}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) { return (uintptr_t)h_nvnBufferMap(b); }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b) { return (NVNmemoryPool *)((const z_buffer *)b)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b) { return ((const z_buffer *)b)->offset; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b) { return ((const z_buffer *)b)->size; }
static void buffer_cohere(const NVNbuffer *b, ptrdiff_t off, size_t size) {
    const z_buffer *v = (const z_buffer *)b;
    if (off < 0 || (size_t)off > v->size || size > v->size - (size_t)off) {
        LOG("BLOCKED NVN buffer cache range out of bounds"); exit(78);
    }
    // GPU completion is ordered by NVN fences; shared Metal storage needs no cache copy.
    atomic_thread_fence(memory_order_seq_cst);
}
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t size) { buffer_cohere(b, off, size); }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t size) { buffer_cohere(b, off, size); }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *memory, ptrdiff_t off, size_t size) {
    const pool_t *pool = (const pool_t *)memory;
    z_buffer view = {.pool = (pool_t *)pool, .size = pool->size};
    buffer_cohere((const NVNbuffer *)&view, off, size);
}
NVN(nvnCommandBufferSetShaderScratchMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *memory, ptrdiff_t offset, size_t size) {
    (void)c;
    const pool_t *pool = (const pool_t *)memory;
    if (!pool || offset < 0 || (uint64_t)offset > pool->size || size > pool->size - (uint64_t)offset) {
        LOG("BLOCKED NVN shader scratch bounds"); exit(78);
    }
    // Maxwell local memory becomes SPIR-V private variables; Metal owns their backing storage.
}
