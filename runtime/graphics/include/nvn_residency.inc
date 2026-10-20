static void res_add(id<MTLResource> resource) {
    if (!resource) return;
    pthread_mutex_lock(&g_res_mu);
    if ([resource conformsToProtocol:@protocol(MTLTexture)]) {
        id<MTLTexture> texture = (id<MTLTexture>)resource;
        while (texture.parentTexture) texture = texture.parentTexture;
        id<MTLResource> allocation = texture.buffer ?: (id<MTLResource>)texture;
        // Views share their parent's resident allocation; registering each view churns the driver set.
        if (allocation != resource && [g_res_objects containsObject:allocation]) {
            pthread_mutex_unlock(&g_res_mu); return;
        }
    }
    if (![g_res_objects containsObject:resource]) {
        [g_res addAllocation:(id<MTLAllocation>)resource];
        if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
        [g_res_objects addObject:resource]; g_res_dirty = 1;
        if (g_res_census < 0) g_res_census = getenv("DM_RES_CENSUS") != NULL;
        if (g_res_census > 0) {
            if (!g_res_ever) g_res_ever = [NSHashTable weakObjectsHashTable];
            [g_res_ever addObject:resource];
        }
    }
    pthread_mutex_unlock(&g_res_mu);
}
static void res_remove(id<MTLResource> resource) {
    if (!resource) return;
    pthread_mutex_lock(&g_res_mu);
    if ([g_res_objects containsObject:resource]) {
        [g_res removeAllocation:(id<MTLAllocation>)resource];
        [g_res_objects removeObject:resource]; g_res_dirty = 1;
    }
    pthread_mutex_unlock(&g_res_mu);
}
// A registered view, any registered ancestor, its backing buffer or heap covers this access.
static int zelda_resource_resident(id resource) {
    pthread_mutex_lock(&g_res_mu);
    int found = 0;
    __unsafe_unretained id current = resource;
    while (current && !found) {
        found = [g_res_objects containsObject:current];
        if (found) break;
        if ([current conformsToProtocol:@protocol(MTLTexture)]) {
            __unsafe_unretained id<MTLTexture> texture = current;
            current = (id)texture.parentTexture ?: (id)texture.buffer ?: (id)texture.heap;
        } else if ([current conformsToProtocol:@protocol(MTLBuffer)]) {
            current = ((id<MTLBuffer>)current).heap;
        } else current = nil;
    }
    pthread_mutex_unlock(&g_res_mu);
    return found;
}
static void zelda_texture_alias_census(void);
// DM_RES_CENSUS breakdown of the resident textures by pixel format and kind (render target, buffer-backed view of
// guest memory, or own storage), largest first, to size what a copy of guest data costs per format.
static void zelda_texture_census(void) {
    NSMutableDictionary<NSString *, NSNumber *> *bytes = [NSMutableDictionary dictionary], *counts = [NSMutableDictionary dictionary];
    pthread_mutex_lock(&g_res_mu);
    for (id object in g_res_objects.allObjects) {
        if (![object conformsToProtocol:@protocol(MTLTexture)]) continue;
        id<MTLTexture> texture = object;
        const char *kind = texture.usage & MTLTextureUsageRenderTarget ? "rt" : texture.buffer ? "buf" : "tex";
        NSString *key = [NSString stringWithFormat:@"%s/%lu", kind, (unsigned long)texture.pixelFormat];
        bytes[key] = @(bytes[key].unsignedLongLongValue + texture.allocatedSize); counts[key] = @(counts[key].unsignedIntValue + 1);
    }
    pthread_mutex_unlock(&g_res_mu);
    NSArray<NSString *> *keys = [bytes keysSortedByValueUsingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) { return [b compare:a]; }];
    NSMutableString *line = [NSMutableString string];
    for (NSString *key in keys) [line appendFormat:@" %@×%u=%.1fM", key, counts[key].unsignedIntValue, bytes[key].unsignedLongLongValue / 1048576.0];
    MLOG("Resident textures by format (type/MTLPixelFormat x count=MiB):%s", line.UTF8String);
    zelda_texture_alias_census();
}
