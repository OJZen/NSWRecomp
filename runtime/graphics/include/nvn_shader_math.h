// Preserve explicit fma rounding while allowing the generated helpers to inline.
static NSString *zelda_shader_math(NSString *source, MTLCompileOptions **options) {
    NSString *result = source;
    NSArray<NSString *> *helpers = @[
        @"T spvFAdd(", @"T spvFSub(", @"T spvFMul(",
        @"vec<T, Cols> spvFMulVectorMatrix(", @"vec<T, Rows> spvFMulMatrixVector(",
        @"matrix<T, RCols, LRows> spvFMulMatrixMatrix("
    ];
    BOOL changed = NO;
    for (NSString *helper in helpers) {
        NSString *declaration = [@"[[clang::optnone]] " stringByAppendingString:helper];
        if ([result rangeOfString:declaration].location == NSNotFound) continue;
        result = [result stringByReplacingOccurrencesOfString:declaration
            withString:[@"inline __attribute__((always_inline)) " stringByAppendingString:helper]];
        changed = YES;
    }
    if (!changed) return source;
    *options = [MTLCompileOptions new];
    (*options).mathMode = MTLMathModeSafe;
    (*options).mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    return [@"#pragma clang fp contract(off)\n" stringByAppendingString:result];
}
