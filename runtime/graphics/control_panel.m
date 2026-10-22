#include "control_panel.h"
#include "performance.h"
#include "gamepad.h"
#include "audio.h"
#include "game_timing.h"
#include <mach/mach.h>
#include <sys/resource.h>
#include <math.h>

enum { FPS, RENDER_FPS, P95, CPU, GPU, CPU_FRAME, SUBMIT, GPU_FRAME, RESOLUTION, DRAWABLE, MEMORY, DRAWS, METRIC_COUNT };
@implementation TotkControlPanel {
    __weak NSWindow *game_window;
    CAMetalLayer *metal_layer;
    NSTextField *values[METRIC_COUNT], *state, *controller, *input;
    NSButton *sound;
    NSSegmentedControl *frame_rate;
    NSTimer *timer;
    double last_time, last_cpu;
    uint64_t last_frame;
}
static NSTextField *label(NSString *text, CGFloat size, NSFontWeight weight) {
    NSTextField *view = [NSTextField labelWithString:text];
    view.font = [NSFont systemFontOfSize:size weight:weight]; return view;
}
static NSString *milliseconds(double value) {
    return value >= 0 && isfinite(value) ? [NSString stringWithFormat:@"%.2f ms", value] : @"—";
}
- (instancetype)initWithGameWindow:(NSWindow *)window layer:(CAMetalLayer *)layer {
    audio_mute([NSUserDefaults.standardUserDefaults boolForKey:@"AudioMuted"]);
    unsigned fps = (unsigned)[NSUserDefaults.standardUserDefaults integerForKey:@"TargetFPS"];
    if (!timing_set_target(fps)) timing_set_target(30);
    const char *override = getenv("TOTK_TARGET_FPS");
    if (override) {
        if (!strcmp(override, "30")) fps = 30;
        else if (!strcmp(override, "60")) fps = 60;
        else if (!strcmp(override, "90")) fps = 90;
        else if (!strcmp(override, "120")) fps = 120;
        else { fputs("TOTK_TARGET_FPS must be 30, 60, 90 or 120\n", stderr); exit(78); }
        timing_set_target(fps);
    }
    fprintf(stderr, "TIMING target=%u movie=30 clock_scale=1\n", timing_target());
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 480, 740)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow
        backing:NSBackingStoreBuffered defer:NO];
    self = [super initWithWindow:panel]; if (!self) return nil;
    game_window = window; metal_layer = layer;
    panel.title = @"控制面板"; panel.releasedWhenClosed = NO; panel.hidesOnDeactivate = YES;
    NSStackView *stack = [NSStackView new]; stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = 12; stack.translatesAutoresizingMaskIntoConstraints = NO;
    [panel.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:20],
        [stack.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:22],
        [stack.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-22],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:panel.contentView.bottomAnchor constant:-20]]];
    [stack addArrangedSubview:label(@"性能监测", 20, NSFontWeightSemibold)];
    state = label(@"等待游戏出帧…", 12, NSFontWeightRegular); state.textColor = NSColor.secondaryLabelColor;
    [stack addArrangedSubview:state];
    frame_rate = [NSSegmentedControl segmentedControlWithLabels:@[@"30", @"60", @"90", @"120"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(frameRateChanged:)];
    frame_rate.selectedSegment = timing_target() / 30 - 1;
    frame_rate.accessibilityLabel = @"目标帧率";
    frame_rate.toolTip = @"立即生效并记住选择；预渲染影片保持 30 FPS。实际帧率取决于负载与显示器。";
    NSStackView *fps_row = [NSStackView stackViewWithViews:@[label(@"目标帧率", 13, NSFontWeightMedium), frame_rate]];
    fps_row.spacing = 14; [stack addArrangedSubview:fps_row];
    NSArray *names = @[@"呈现帧率", @"游戏出帧率", @"帧间隔 P95", @"CPU 占用 · 本进程", @"GPU 占用 · 忙碌估算",
        @"CPU / 帧 · 全进程", @"CPU / 帧 · 图形提交", @"GPU / 帧 · 命令耗时", @"游戏渲染分辨率", @"窗口输出分辨率", @"物理内存", @"绘制调用 / 帧"];
    NSMutableArray *rows = [NSMutableArray new];
    for (unsigned i = 0; i < METRIC_COUNT; i++) {
        NSTextField *name = label(names[i], 13, NSFontWeightRegular);
        values[i] = label(@"—", 13, NSFontWeightMedium);
        values[i].font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
        values[i].alignment = NSTextAlignmentRight; values[i].accessibilityLabel = names[i];
        [rows addObject:@[name, values[i]]];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows]; grid.rowSpacing = 9; grid.columnSpacing = 14;
    [grid columnAtIndex:0].width = 220; [grid columnAtIndex:1].xPlacement = NSGridCellPlacementTrailing;
    [stack addArrangedSubview:grid]; [grid.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    values[CPU].toolTip = @"所有线程 CPU 时间 / 墙钟时间；100% 表示占满一个 CPU 核，可超过 100%。";
    values[GPU].toolTip = @"最近 2 秒已完成的图形命令缓冲 GPU 时间区间去重后的占比；不代表整机 GPU 或着色器核心利用率。";
    values[CPU_FRAME].toolTip = @"采样期内全进程 CPU 时间之和 / 游戏出帧数；包含并行线程，不是帧延迟。";
    values[SUBMIT].toolTip = @"图形翻译提交线程实际 CPU 时间，不含等待和出图；最近 120 帧均值。";
    values[GPU_FRAME].toolTip = @"每帧已完成命令缓冲 GPU 时间之和；并行区间可能重叠。最多滞后 8 帧，缺失读数不当作 0。";
    NSTextField *note = [NSTextField wrappingLabelWithString:@"帧指标：最近 120 帧；CPU：最近 1 秒；GPU 忙碌：最近 2 秒。\nCPU 100% = 1 核；GPU 仅统计本进程已完成的图形命令。"];
    note.font = [NSFont systemFontOfSize:11]; note.textColor = NSColor.secondaryLabelColor;
    [stack addArrangedSubview:note]; [note.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [stack addArrangedSubview:label(@"声音与手柄", 16, NSFontWeightSemibold)];
    sound = [NSButton checkboxWithTitle:@"播放游戏声音" target:self action:@selector(soundChanged:)];
    sound.toolTip = @"立即生效并记住选择。";
    sound.state = audio_muted() ? NSControlStateValueOff : NSControlStateValueOn;
    const char *setting = getenv("DM_AUDIO");
    if (setting && !strcmp(setting, "0")) { sound.enabled = NO; sound.state = NSControlStateValueOff; sound.title = @"启动参数 DM_AUDIO=0 已禁用声音设备"; }
    [stack addArrangedSubview:sound];
    controller = label(@"", 13, NSFontWeightMedium); [stack addArrangedSubview:controller];
    input = [NSTextField wrappingLabelWithString:@" "];
    input.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    [stack addArrangedSubview:input]; [input.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    NSTextField *mapping = [NSTextField wrappingLabelWithString:@"Xbox A/B/X/Y 按字母映射；Menu = +，View = −。\n左摇杆移动，右摇杆转镜头；摇杆死区 15%。\n切回游戏窗口恢复控制，失焦或断开时释放输入。"];
    mapping.font = [NSFont systemFontOfSize:11]; mapping.textColor = NSColor.secondaryLabelColor;
    [stack addArrangedSubview:mapping]; [mapping.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    NSButton *resume = [NSButton buttonWithTitle:@"返回游戏" target:self action:@selector(returnToGame:)];
    [stack addArrangedSubview:resume];
    __weak TotkControlPanel *weak_self = self;
    timer = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) { (void)t; [weak_self refresh]; }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    [panel center]; [self refresh]; return self;
}
- (void)dealloc { [timer invalidate]; }
- (void)showPanel:(id)sender { (void)sender; [self refresh]; [self.window makeKeyAndOrderFront:nil]; }
- (void)returnToGame:(id)sender { (void)sender; [game_window makeKeyAndOrderFront:nil]; [game_window makeFirstResponder:game_window.contentView]; }
- (void)soundChanged:(NSButton *)sender {
    bool muted = sender.state != NSControlStateValueOn;
    audio_mute(muted);
    [NSUserDefaults.standardUserDefaults setBool:muted forKey:@"AudioMuted"];
}
- (void)frameRateChanged:(NSSegmentedControl *)sender {
    unsigned fps = (unsigned)(sender.selectedSegment + 1) * 30;
    if (!timing_set_target(fps)) return;
    [NSUserDefaults.standardUserDefaults setInteger:fps forKey:@"TargetFPS"];
    performance_reset_window();
    fprintf(stderr, "TIMING target=%u source=control_panel\n", fps);
    [self refresh];
}
- (void)refresh {
    ZeldaPerformance p; bool ready = performance_snapshot(&p);
    double now = CACurrentMediaTime(); bool live = ready && now - p.last_rendered < 2;
    state.stringValue = live ? [NSString stringWithFormat:@"实时 · %u 帧采样 · 目标 %.0f FPS", p.samples, p.pacing_fps]
                                  : (ready ? @"出帧已停止 · 保留最后分辨率" : @"等待游戏出帧…");
    values[FPS].stringValue = live && p.fps >= 0 && now - p.last_presented < 2 ? [NSString stringWithFormat:@"%.1f FPS", p.fps] : @"—";
    values[RENDER_FPS].stringValue = live ? [NSString stringWithFormat:@"%.1f FPS", p.render_fps] : @"—";
    values[P95].stringValue = live && p.fps >= 0 ? milliseconds(p.frame_p95_ms) : @"—";
    values[SUBMIT].stringValue = live ? milliseconds(p.submit_cpu_ms) : @"—";
    values[GPU_FRAME].stringValue = live ? milliseconds(p.gpu_ms) : @"—";
    double busy = performance_gpu_busy(now);
    values[GPU].stringValue = busy >= 0 ? [NSString stringWithFormat:@"%.1f %%", busy] : @"—";
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage)) {
        double cpu = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        if (last_time && now - last_time >= .5 && cpu >= last_cpu) {
            values[CPU].stringValue = [NSString stringWithFormat:@"%.1f %%", (cpu - last_cpu) / (now - last_time) * 100];
            values[CPU_FRAME].stringValue = live && p.frame > last_frame ? milliseconds((cpu - last_cpu) * 1000 / (p.frame - last_frame)) : @"—";
        }
        // A manual refresh must not shrink the one-second CPU observation window.
        if (!last_time || now - last_time >= .5) { last_time = now; last_cpu = cpu; last_frame = p.frame; }
    } else { values[CPU].stringValue = values[CPU_FRAME].stringValue = @"—"; }
    values[RESOLUTION].stringValue = p.render_width ? [NSString stringWithFormat:@"%u × %u", p.render_width, p.render_height] : @"—";
    CGSize size = metal_layer.drawableSize;
    values[DRAWABLE].stringValue = [NSString stringWithFormat:@"%.0f × %.0f", size.width, size.height];
    task_vm_info_data_t info; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    values[MEMORY].stringValue = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS
        ? [NSString stringWithFormat:@"%.2f GiB", info.phys_footprint / 1073741824.0] : @"—";
    values[DRAWS].stringValue = live ? [NSString stringWithFormat:@"%.0f", p.draws] : @"—";
    controller.stringValue = gamepad_name(); input.stringValue = gamepad_readout();
    if (sound.enabled) sound.state = audio_muted() ? NSControlStateValueOff : NSControlStateValueOn;
}
@end
