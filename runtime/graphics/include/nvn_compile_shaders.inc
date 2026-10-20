#include <TargetConditionals.h>
static void zelda_compile_shaders(NSArray<NSString *> *keys) {
#if TARGET_OS_IPHONE
    // Captured requests remain in Documents/work/shaders for the Mac preparation tool.
    MDIE("Shader translation is required on the Mac; export work/shaders requests, prepare them, and sync back to iOS");
#else
    @autoreleasepool {
        NSString *root = NSFileManager.defaultManager.currentDirectoryPath;
        NSString *log = [root stringByAppendingPathComponent:@"work/logs/runtime-shaders.log"];
        if (![NSFileManager.defaultManager createFileAtPath:log contents:nil attributes:nil])
            MDIE("Cannot create runtime shader build log");
        NSFileHandle *output = [NSFileHandle fileHandleForWritingAtPath:log];
        if (!output) MDIE("Cannot open runtime shader build log");
        NSTask *task = [NSTask new];
        task.executableURL = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:@"work/venv/bin/python"]];
        if (!keys.count) MDIE("Empty runtime shader request");
        task.arguments = [@[@"tools/compile_runtime_shaders.py"] arrayByAddingObjectsFromArray:keys];
        NSMutableDictionary *environment = [NSProcessInfo.processInfo.environment mutableCopy];
        environment[@"PYTHONDONTWRITEBYTECODE"] = @"1";
        environment[@"TMPDIR"] = [root stringByAppendingPathComponent:@"work/tmp/"];
        task.environment = environment; task.standardOutput = output; task.standardError = output;
        MLOG("Preparing captured shaders; details: work/logs/runtime-shaders.log");
        NSError *error = nil;
        if (![task launchAndReturnError:&error]) MDIE("Cannot launch local shader compiler");
        [task waitUntilExit]; [output closeFile];
        if (task.terminationReason != NSTaskTerminationReasonExit || task.terminationStatus)
            MDIE("Runtime shader compilation failed; inspect work/logs/runtime-shaders.log");
    }
#endif
}
