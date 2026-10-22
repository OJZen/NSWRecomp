#include "../runtime/graphics/control_panel.m"
#include "cpu.h"
#include <assert.h>

static bool muted;
bool audio_muted(void) { return muted; }
void audio_mute(bool value) { muted = value; }
NSString *gamepad_name(void) { return @"No controller"; }
NSString *gamepad_readout(void) { return @""; }
void rt_udf(Cpu *c, uint64_t site) { (void)c; (void)site; abort(); }
static NSSegmentedControl *selector(NSView *view) {
    if ([view isKindOfClass:NSSegmentedControl.class]) return (NSSegmentedControl *)view;
    for (NSView *child in view.subviews) { NSSegmentedControl *found = selector(child); if (found) return found; }
    return nil;
}
static NSButton *sound_switch(NSView *view) {
    if ([view isKindOfClass:NSButton.class] && [(NSButton *)view action] == @selector(soundChanged:)) return (NSButton *)view;
    for (NSView *child in view.subviews) { NSButton *found = sound_switch(child); if (found) return found; }
    return nil;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        id previous = [defaults objectForKey:@"TargetFPS"];
        id previous_mute = [defaults objectForKey:@"AudioMuted"];
        unsetenv("TOTK_TARGET_FPS");
        unsetenv("DM_AUDIO");
        if (argc == 3) {
            assert(!strcmp(argv[1], "--verify-saved-settings"));
            bool expected_mute = !strcmp(argv[2], "muted");
            assert([defaults integerForKey:@"TargetFPS"] == 120);
            assert([defaults boolForKey:@"AudioMuted"] == expected_mute);
            TotkControlPanel *reopened = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            assert(timing_target() == 120 && selector(reopened.window.contentView).selectedSegment == 3);
            assert(audio_muted() == expected_mute);
            assert(sound_switch(reopened.window.contentView).state == (expected_mute ? NSControlStateValueOff : NSControlStateValueOn));
            return 0;
        }
        @try {
            [defaults setInteger:30 forKey:@"TargetFPS"];
            [defaults removeObjectForKey:@"AudioMuted"]; audio_mute(true);
            TotkControlPanel *panel = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            NSButton *sound = sound_switch(panel.window.contentView);
            assert(sound && sound.enabled && sound.state == NSControlStateValueOn && !audio_muted());
            NSSegmentedControl *control = selector(panel.window.contentView);
            assert(control && control.segmentCount == 4 && control.selectedSegment == 0);
            for (NSInteger i = 0; i < 4; i++) {
                control.selectedSegment = i;
                assert([NSApp sendAction:control.action to:control.target from:control]);
                assert(timing_target() == (unsigned)(i + 1) * 30);
                assert([defaults integerForKey:@"TargetFPS"] == timing_target());
            }
            TotkControlPanel *reopened = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            assert(selector(reopened.window.contentView).selectedSegment == 3);
            for (int value = 1; value >= 0; value--) {
                sound.state = value ? NSControlStateValueOff : NSControlStateValueOn;
                assert([NSApp sendAction:sound.action to:sound.target from:sound]);
                assert(audio_muted() == value && [defaults boolForKey:@"AudioMuted"] == value);
                assert([defaults synchronize]);
                NSTask *relaunch = [NSTask new];
                relaunch.executableURL = [NSURL fileURLWithPath:@(argv[0])];
                relaunch.arguments = @[@"--verify-saved-settings", value ? @"muted" : @"audible"];
                assert([relaunch launchAndReturnError:nil]);
                [relaunch waitUntilExit];
                assert(relaunch.terminationStatus == 0);
            }
            setenv("DM_AUDIO", "0", 1);
            TotkControlPanel *disabled = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            NSButton *disabled_sound = sound_switch(disabled.window.contentView);
            assert(!disabled_sound.enabled && disabled_sound.state == NSControlStateValueOff);
            assert(![defaults boolForKey:@"AudioMuted"]);
            unsetenv("DM_AUDIO");
            TotkControlPanel *enabled = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            assert(sound_switch(enabled.window.contentView).state == NSControlStateValueOn && !audio_muted());
            setenv("TOTK_TARGET_FPS", "90", 1);
            TotkControlPanel *override = [[TotkControlPanel alloc] initWithGameWindow:nil layer:nil];
            assert(selector(override.window.contentView).selectedSegment == 2 && timing_target() == 90);
            assert([defaults integerForKey:@"TargetFPS"] == 120);
        } @finally {
            if (previous) [defaults setObject:previous forKey:@"TargetFPS"];
            else [defaults removeObjectForKey:@"TargetFPS"];
            if (previous_mute) [defaults setObject:previous_mute forKey:@"AudioMuted"];
            else [defaults removeObjectForKey:@"AudioMuted"];
            assert([defaults synchronize]);
        }
    }
}
