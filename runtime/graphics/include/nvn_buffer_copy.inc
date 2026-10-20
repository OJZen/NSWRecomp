static inline void enc_b_copy_buffer(id encoder, id<MTLBuffer> source, NSUInteger sourceOffset,
                                    id<MTLBuffer> destination, NSUInteger destinationOffset, NSUInteger size) {
    // Metal 4 copies may overlap; NVN's later copy must observe earlier writes, including aliases.
    if ([encoder conformsToProtocol:@protocol(MTL4CommandEncoder)])
        [encoder barrierAfterEncoderStages:(MTLStageBlit | MTLStageDispatch) beforeEncoderStages:MTLStageBlit
            visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromBuffer:source sourceOffset:sourceOffset toBuffer:destination destinationOffset:destinationOffset size:size];
}
