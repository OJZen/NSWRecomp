#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "host_compat.h"
#include "shader_prep.h"
#include "input.h"
#include "gamepad.h"
#include "control_panel.h"
#include <stdatomic.h>
#include <mach/mach.h>

static CAMetalLayer *layer;
static void (*wait_dump)(void);
static dispatch_source_t vsync;
static uint32_t vsync_write, vsync_read;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_source_t memory_watch;
uint64_t g_t0_ns;
int startup_run(int argc, char **argv);
void startup_window_closed(void);

void die(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr); if (wait_dump) wait_dump(); exit(78);
}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void *front_metal_layer(void) { return (__bridge void *)layer; }
void crash_set_wait_dump(void (*fn)(void)) { wait_dump = fn; }
void dm_res_report_register(void (*fn)(uint32_t, const char *)) { (void)fn; }
uint32_t front_vsync_handle(void) {
    pthread_mutex_lock(&vsync_lock);
    if (!vsync && kernel_event_create(&vsync_write, &vsync_read)) {
        vsync = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        if (!vsync) die("Cannot create VI refresh clock");
        dispatch_source_set_timer(vsync, DISPATCH_TIME_NOW, NSEC_PER_SEC / 60, 100000);
        dispatch_source_set_event_handler(vsync, ^{
            uint64_t registers[32] = {vsync_write};
            kernel_svc(0x11, registers);
        });
        dispatch_resume(vsync);
        LOG("VI refresh event: 60 Hz");
    }
    uint32_t handle = vsync ? kernel_event_copy(vsync_read) : 0;
    pthread_mutex_unlock(&vsync_lock);
    return handle;
}
// Optional precompiled IR is absent; Metal compiles the translated source with the same options.
id<MTLLibrary> shader_ir_load(id<MTLDevice> device, NSString *path, const void *source, size_t length, bool layered) {
    (void)device; (void)path; (void)source; (void)length; (void)layered; return nil;
}
void shader_ir_report(const char *phase) { (void)phase; }
static atomic_uint shader_builds;
static pthread_mutex_t shader_compile_lock = PTHREAD_MUTEX_INITIALIZER;
void shader_status_note(unsigned event, unsigned total) {
    (void)total;
    if (event == SHADER_BUILD_BEGIN) { pthread_mutex_lock(&shader_compile_lock); shader_builds++; }
    if (event == SHADER_BUILD_END) { shader_builds--; pthread_mutex_unlock(&shader_compile_lock); }
}
ZeldaShaderStatus shader_status_snapshot(void) { return (ZeldaShaderStatus){.active_builds = atomic_load(&shader_builds)}; }

@interface TotkDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) TotkControlPanel *panel;
@end
@implementation TotkDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
- (void)applicationWillTerminate:(NSNotification *)notification { (void)notification; gamepad_stop(); startup_window_closed(); }
- (void)applicationWillResignActive:(NSNotification *)notification { (void)notification; gamepad_active(false); }
- (void)applicationDidBecomeActive:(NSNotification *)notification { (void)notification; gamepad_active(self.window.isKeyWindow); }
- (void)windowWillClose:(NSNotification *)notification { (void)notification; [NSApp terminate:nil]; }
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    NSView *view = self.window.contentView;
    layer.contentsScale = self.window.backingScaleFactor;
    layer.drawableSize = [view convertSizeToBacking:view.bounds.size];
}
- (void)windowDidChangeBackingProperties:(NSNotification *)notification { [self windowDidResize:notification]; }
- (void)windowDidResignKey:(NSNotification *)notification { (void)notification; gamepad_active(false); }
- (void)windowDidBecomeKey:(NSNotification *)notification { (void)notification; gamepad_active(NSApp.isActive); }
@end

@interface TotkView : NSView
@end
@implementation TotkView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event { if (!(event.modifierFlags & NSEventModifierFlagCommand)) input_key(event.keyCode, 1); }
- (void)keyUp:(NSEvent *)event { input_key(event.keyCode, 0); }
- (void)flagsChanged:(NSEvent *)event {
    if (event.keyCode == 56 || event.keyCode == 60) input_key(56, !!(event.modifierFlags & NSEventModifierFlagShift));
}
@end

static int saved_argc;
static char **saved_argv;
static void *game_thread(void *unused) {
    (void)unused;
    @autoreleasepool { exit(startup_run(saved_argc, saved_argv)); }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        g_t0_ns = now_ns(); saved_argc = argc; saved_argv = argv;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        static TotkDelegate *delegate;
        delegate = [TotkDelegate new]; NSApp.delegate = delegate;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        delegate.window = window; window.delegate = delegate; window.title = @"TOTK Recompiled";
        window.contentView = [[TotkView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
        layer = [CAMetalLayer layer]; layer.device = MTLCreateSystemDefaultDevice();
        if (!layer.device) die("Metal device unavailable");
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm; layer.framebufferOnly = YES;
        window.contentView.wantsLayer = YES; window.contentView.layer = layer;
        delegate.panel = [[TotkControlPanel alloc] initWithGameWindow:window layer:layer];
        NSTitlebarAccessoryViewController *accessory = [NSTitlebarAccessoryViewController new];
        NSButton *controls = [NSButton buttonWithTitle:@"控制面板" target:delegate.panel action:@selector(showPanel:)];
        controls.frame = NSMakeRect(0, 0, 90, 26); controls.toolTip = @"性能、声音与手柄（⌘,）";
        accessory.view = controls; accessory.layoutAttribute = NSLayoutAttributeRight;
        [window addTitlebarAccessoryViewController:accessory];
        [delegate windowDidResize:nil]; [window center]; [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:window.contentView];
        [NSApp activateIgnoringOtherApps:YES];
        LOG("AppKit window=%ld visible=%d bundle=%s process_bundle=%s", (long)window.windowNumber, window.isVisible,
            (NSBundle.mainBundle.bundleIdentifier ?: @"unknown").UTF8String,
            (NSRunningApplication.currentApplication.bundleIdentifier ?: @"unregistered").UTF8String);
        NSMenu *menu = [NSMenu new]; NSMenuItem *item = [NSMenuItem new]; [menu addItem:item];
        NSMenu *app = [NSMenu new];
        NSMenuItem *panel_item = [app addItemWithTitle:@"控制面板…" action:@selector(showPanel:) keyEquivalent:@","];
        panel_item.target = delegate.panel;
        [app addItem:NSMenuItem.separatorItem];
        [app addItemWithTitle:@"Quit TOTK Recompiled" action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = app; NSApp.mainMenu = menu;
        gamepad_start(); gamepad_active(window.isKeyWindow && NSApp.isActive);
        memory_watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                              dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        if (!memory_watch) die("Cannot create the native memory guard");
        dispatch_source_set_timer(memory_watch, DISPATCH_TIME_NOW, NSEC_PER_SEC / 2, 10000000);
        dispatch_source_set_event_handler(memory_watch, ^{
            task_vm_info_data_t info; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
                die("Native physical memory monitoring unavailable");
            if (info.phys_footprint > (4ull << 30)) die("Native physical memory exceeded 4 GiB");
        });
        dispatch_resume(memory_watch);
        pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64 << 20);
        pthread_t thread; int error = pthread_create(&thread, &attr, game_thread, NULL);
        pthread_attr_destroy(&attr); if (error) die("Cannot start guest worker: %d", error);
        pthread_detach(thread);
        [NSApp run];
    }
    return 0;
}
