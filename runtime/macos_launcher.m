#import <Cocoa/Cocoa.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static void launch_error(NSString *message) {
    fprintf(stderr, "TOTK launch failed: %s\n", message.UTF8String);
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"无法启动 TOTK Recompiled";
    alert.informativeText = message; [alert runModal];
}

@interface TotkLauncher : NSObject <NSApplicationDelegate>
@end
@implementation TotkLauncher
- (BOOL)applicationShouldHandleReopen:(NSApplication *)application hasVisibleWindows:(BOOL)visible {
    (void)application; (void)visible;
    NSString *game_id = [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".runtime"];
    for (NSRunningApplication *app in [NSRunningApplication runningApplicationsWithBundleIdentifier:game_id])
        if (app.processIdentifier != NSProcessInfo.processInfo.processIdentifier) [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    return YES;
}
@end

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        static TotkLauncher *delegate; delegate = [TotkLauncher new]; NSApp.delegate = delegate;
        NSURL *root = NSBundle.mainBundle.bundleURL;
        for (unsigned i = 0; i < 3; i++) root = root.URLByDeletingLastPathComponent;
        NSString *script = [root.path stringByAppendingPathComponent:@"script/build_and_run.sh"];
        NSString *log = [root.path stringByAppendingPathComponent:@"work/logs/app-launch.log"];
        if (![NSFileManager.defaultManager isExecutableFileAtPath:script]) {
            launch_error(@"无法读取工作区启动脚本。请保留应用在 work/app 中，并允许它访问外置盘。"); return 1;
        }
        int fd = open(log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) { launch_error([NSString stringWithFormat:@"无法写入启动日志：%s\n%@", strerror(errno), log]); return 1; }
        NSFileHandle *output = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
        NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
        task.arguments = @[script, @"--seconds", @"600"]; task.currentDirectoryURL = root;
        NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
        env[@"PATH"] = [(env[@"PATH"] ?: @"/usr/bin:/bin:/usr/sbin:/sbin") stringByAppendingString:@":/opt/homebrew/bin:/usr/local/bin"];
        task.environment = env; task.standardOutput = output; task.standardError = output;
        task.standardInput = NSFileHandle.fileHandleWithNullDevice;
        task.terminationHandler = ^(NSTask *finished) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (finished.terminationStatus) launch_error([NSString stringWithFormat:@"启动或运行已停止（状态 %d）。\n请查看日志：%@", finished.terminationStatus, log]);
                [NSApp stop:nil];
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                    timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
            });
        };
        NSError *error = nil;
        if (![task launchAndReturnError:&error]) { launch_error(error.localizedDescription); return 1; }
        // Retain the native application identity while its children read the workspace on the external volume.
        [NSApp run]; return task.terminationStatus;
    }
}
