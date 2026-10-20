#include <math.h>

static void zelda_attachmentless_pass(MTL4RenderPassDescriptor *pass, const float viewport[4], int has_viewport) {
    double width = has_viewport ? ceil((double)viewport[0] + viewport[2]) : 1;
    double height = has_viewport ? ceil((double)viewport[1] + viewport[3]) : 1;
    if (!isfinite(width) || !isfinite(height) || width > 16384 || height > 16384)
        MDIE("Invalid attachmentless viewport extent");
    pass.renderTargetWidth = (NSUInteger)MAX(width, 1);
    pass.renderTargetHeight = (NSUInteger)MAX(height, 1);
    pass.defaultRasterSampleCount = 1;
}
