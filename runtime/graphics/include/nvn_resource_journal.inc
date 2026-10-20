#include <objc/runtime.h>

// Object generations survive view changes without retaining resources or trusting reused pointer values.
static uint64_t zj_identity(id object) {
    if (!object) return 0;
    static char key;
    static _Atomic uint64_t next;
    @synchronized (object) {
        NSNumber *number = objc_getAssociatedObject(object, &key);
        if (!number) { number = @(atomic_fetch_add(&next, 1) + 1); objc_setAssociatedObject(object, &key, number, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
        return number.unsignedLongLongValue;
    }
}
static void zj_texture_info(zj_event *event, id<MTLTexture> texture) {
    event->v[ZJ_LEVEL] = event->v[ZJ_LAYER] = 0;
    event->v[ZJ_RESOURCE] = zj_identity(texture);
    id<MTLTexture> root = texture;
    unsigned parents = 0;
    while (root.parentTexture && parents++ < 32) {
        event->v[ZJ_LEVEL] += root.parentRelativeLevel; event->v[ZJ_LAYER] += root.parentRelativeSlice;
        root = root.parentTexture;
    }
    if (root.parentTexture) { event->v[ZJ_DETAIL] |= 1; return; }
    event->v[ZJ_STORAGE] = zj_identity(root); event->v[ZJ_HEAP] = zj_identity(root.heap);
    event->v[ZJ_HEAP_OFFSET] = root.heap ? root.heapOffset : 0; event->v[ZJ_BUFFER] = zj_identity(root.buffer);
    event->v[ZJ_OFFSET] = root.bufferOffset; event->v[ZJ_STORAGE_BYTES] = root.allocatedSize;
    event->v[ZJ_LEVELS] = texture.mipmapLevelCount;
    event->v[ZJ_LAYERS] = texture.textureType == MTLTextureType3D ? texture.depth :
        texture.arrayLength * ((texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray) ? 6 : 1);
    event->v[ZJ_TYPE] = texture.textureType; event->v[ZJ_FORMAT] = texture.pixelFormat;
    event->v[ZJ_WIDTH] = texture.width; event->v[ZJ_HEIGHT] = texture.height; event->v[ZJ_DEPTH] = texture.depth;
    event->v[ZJ_SAMPLES] = texture.sampleCount;
    MTLTextureSwizzleChannels s = texture.swizzle;
    event->v[ZJ_SWIZZLE] = s.red | (s.green << 8) | (s.blue << 16) | (s.alpha << 24);
}

static void zj_texture_range(zj_event *event, id<MTLTexture> texture, NSUInteger level, NSUInteger layer,
                             NSUInteger layers, MTLOrigin origin, MTLSize size) {
    zj_texture_info(event, texture);
    event->v[ZJ_LEVEL] += level; event->v[ZJ_LEVELS] = 1;
    event->v[ZJ_LAYER] += texture.textureType == MTLTextureType3D ? origin.z : layer;
    event->v[ZJ_LAYERS] = texture.textureType == MTLTextureType3D ? size.depth : layers;
    event->v[ZJ_X] = origin.x; event->v[ZJ_Y] = origin.y; event->v[ZJ_Z] = origin.z;
    event->v[ZJ_NX] = size.width; event->v[ZJ_NY] = size.height; event->v[ZJ_NZ] = size.depth;
}
static void zj_buffer_info(zj_event *event, id<MTLBuffer> buffer, uint64_t address, uint64_t length) {
    event->v[ZJ_RESOURCE] = event->v[ZJ_STORAGE] = zj_identity(buffer);
    event->v[ZJ_STORAGE_BYTES] = buffer.length; event->v[ZJ_HEAP] = zj_identity(buffer.heap);
    event->v[ZJ_HEAP_OFFSET] = buffer.heap ? buffer.heapOffset : 0;
    event->v[ZJ_OFFSET] = buffer && address >= buffer.gpuAddress && address - buffer.gpuAddress < buffer.length ? address - buffer.gpuAddress : UINT64_MAX;
    if (event->v[ZJ_OFFSET] != UINT64_MAX && length <= buffer.length - event->v[ZJ_OFFSET]) event->v[ZJ_LENGTH] = length;
}

#ifndef ZELDA_RESOURCE_JOURNAL_METADATA_ONLY
static id<MTLBuffer> zj_bound_buffer(id<MTLBuffer>, uint64_t);
static uint64_t zj_bound_length(const nvn_rbind *, unsigned);
static zj_event zj_context(unsigned kind, unsigned access) {
    zj_event event = {0}; event.v[ZJ_KIND] = kind; event.v[ZJ_ACCESS] = access;
    event.v[ZJ_SUBMIT] = g_sub_serial; event.v[ZJ_PASS] = g_stat_pass;
    event.v[ZJ_GENERATION] = atomic_load(&z_pool_generation);
    event.v[ZJ_DIRTY_EPOCH] = nvn_dirty_now(); event.v[ZJ_FENCE] = g_fence + 1;
    event.v[ZJ_COMPLETED] = atomic_load(&zj_completed);
    event.v[ZJ_COMPLETION_ERROR] = atomic_load(&zj_completion_error);
    return event;
}
static void zj_texture(unsigned kind, unsigned access, const tex_t *guest, id<MTLTexture> texture,
                       unsigned stage, unsigned reg, unsigned expected, const char *shader, id encoder) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, access); zj_texture_info(&event, texture);
    event.v[ZJ_ENCODER] = zj_identity(encoder);
    event.v[ZJ_STAGE] = stage; event.v[ZJ_REG] = reg; event.v[ZJ_EXPECTED_TYPE] = expected;
    if (guest) {
        event.v[ZJ_SERIAL] = guest->serial; event.v[ZJ_SIZE] = guest->L.size;
        event.v[ZJ_ADDRESS] = guest->mem ? nvn_canonical_address((uint64_t)guest->mem, guest->L.size) : 0;
    }
    if (shader) strlcpy(event.shader, shader, sizeof event.shader);
    zj_record(event);
}
static void zj_binding(id<MTLResource> resource, unsigned access, const nvn_rbind *binding,
                       unsigned stage, uint64_t address, const char *shader, id encoder) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || ![resource conformsToProtocol:@protocol(MTLBuffer)] || !zj_capacity()) return;
    id<MTLBuffer> buffer = zj_bound_buffer((id)resource, address);
    zj_event event = zj_context(ZJ_BUFFER_BIND, access);
    zj_buffer_info(&event, buffer, address, zj_bound_length(binding, stage));
    event.v[ZJ_ENCODER] = zj_identity(encoder);
    event.v[ZJ_STAGE] = stage; event.v[ZJ_REG] = binding->reg; event.v[ZJ_DETAIL] = binding->kind;
    strlcpy(event.shader, shader, sizeof event.shader); zj_record(event);
}
static void zj_marker(unsigned kind, uint64_t detail) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, 0); event.v[ZJ_DETAIL] = detail; zj_record(event);
}
static void zj_arguments(unsigned kind, unsigned stage, uint64_t address, const char *shader, id encoder) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, 0); event.v[ZJ_STAGE] = stage; event.v[ZJ_OFFSET] = address;
    event.v[ZJ_ENCODER] = zj_identity(encoder);
    strlcpy(event.shader, shader, sizeof event.shader); zj_record(event);
}
static void zj_command(id encoder, unsigned kind, unsigned detail, MTLSize counts) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, 0); event.v[ZJ_ENCODER] = zj_identity(encoder); event.v[ZJ_DETAIL] = detail;
    event.v[ZJ_NX] = counts.width; event.v[ZJ_NY] = counts.height; event.v[ZJ_NZ] = counts.depth; zj_record(event);
}
static void zj_pipeline(id encoder, id pipeline, unsigned stage) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(ZJ_PIPELINE, 0); event.v[ZJ_ENCODER] = zj_identity(encoder);
    event.v[ZJ_RESOURCE] = zj_identity(pipeline); event.v[ZJ_STAGE] = stage; zj_record(event);
}
static void zj_copy_texture(id encoder, unsigned kind, id<MTLTexture> texture, NSUInteger level, NSUInteger layer, MTLOrigin origin, MTLSize size) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, kind == ZJ_COPY_READ ? ZJ_READ : ZJ_MAY_WRITE);
    zj_texture_range(&event, texture, level, layer, 1, origin, size); event.v[ZJ_ENCODER] = zj_identity(encoder); zj_record(event);
}
static void zj_copy_buffer(id encoder, unsigned kind, id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length) {
    if (!atomic_load(&zj_active) || !on_sub_thread() || !zj_capacity()) return;
    zj_event event = zj_context(kind, kind == ZJ_COPY_READ ? ZJ_READ : ZJ_MAY_WRITE);
    zj_buffer_info(&event, buffer, buffer.gpuAddress + offset, length); event.v[ZJ_ENCODER] = zj_identity(encoder); zj_record(event);
}
static void zj_render_pass(id encoder, id descriptor) {
    if (!atomic_load(&zj_active) || !on_sub_thread()) return;
    zj_command(encoder, ZJ_OPERATION, 1, MTLSizeMake(0,0,0));
    for (unsigned i = 0; i < 10; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < 8 ? [descriptor colorAttachments][i] : i == 8 ? [descriptor depthAttachment] : [descriptor stencilAttachment];
        if (!a.texture) continue;
        unsigned access = ZJ_MAY_WRITE | (a.loadAction == MTLLoadActionLoad ? ZJ_READ : a.loadAction == MTLLoadActionClear ? ZJ_CLEAR : 0);
        if (!zj_capacity()) continue;
        zj_event event = zj_context(ZJ_ATTACHMENT, access); event.v[ZJ_ENCODER] = zj_identity(encoder); event.v[ZJ_REG] = i;
        NSUInteger layers = MAX((NSUInteger)[descriptor renderTargetArrayLength], 1);
        zj_texture_range(&event, a.texture, a.level, a.slice, layers, MTLOriginMake(0,0,a.depthPlane),
            MTLSizeMake(MAX(a.texture.width >> a.level, 1), MAX(a.texture.height >> a.level, 1), a.texture.textureType == MTLTextureType3D ? layers : 1));
        zj_record(event);
        if (a.resolveTexture) zj_marker(ZJ_GAP, 2);
    }
}
#endif
