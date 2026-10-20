#pragma once
static NSString *zelda_cube_source(NSString *input) {
    NSString *source = input;
    NSArray<NSString *> *markers = @[@"struct main0_out\n{", @"vertex main0_out main0(", @"    return out;"];
    for (NSString *marker in markers) if ([source componentsSeparatedByString:marker].count != 2) return nil;
    // Layered pipelines are triangle-only; Metal rejects an unused point-size output there.
    source = [source stringByReplacingOccurrencesOfString:@"    float gl_PointSize [[point_size]];" withString:@""];
    source = [source stringByReplacingOccurrencesOfString:@"    out.gl_PointSize = zelda_point_size;" withString:@""];
    source = [source stringByReplacingOccurrencesOfString:markers[0] withString:@"struct main0_out\n{\n    uint zelda_layer [[render_target_array_index]];"];
    source = [source stringByReplacingOccurrencesOfString:markers[1] withString:@"vertex main0_out main0(constant uint& zelda_layer [[buffer(14)]], "];
    return [source stringByReplacingOccurrencesOfString:markers[2] withString:@"    out.zelda_layer = zelda_layer;\n    return out;"];
}
