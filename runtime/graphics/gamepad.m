// Adapted from Zelda-Mobile host/gamepad.m (Apache-2.0); see third_party/zelda-mobile/LICENSE.
#import <GameController/GameController.h>
#include "gamepad.h"
#include "input.h"
#include <stdio.h>

static GCController *selected;
static id connect_observer, disconnect_observer;
static bool active;
static uint64_t recent_buttons;

static uint64_t buttons(GCExtendedGamepad *pad) {
    GCControllerButtonInput *controls[] = {
        pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY,
        pad.leftThumbstickButton, pad.rightThumbstickButton,
        pad.leftShoulder, pad.rightShoulder, pad.leftTrigger, pad.rightTrigger,
        pad.buttonMenu, pad.buttonOptions, pad.dpad.left, pad.dpad.up, pad.dpad.right, pad.dpad.down,
    };
    uint64_t result = 0;
    for (unsigned i = 0; i < sizeof controls / sizeof *controls; i++) if (controls[i].pressed) result |= 1ull << i;
    return result;
}
static void publish(GCController *controller, GCExtendedGamepad *pad) {
    if (controller != selected) return;
    recent_buttons |= buttons(pad); // Keep brief taps visible until the panel's next refresh.
    if (!active) return;
    input_controller(buttons(pad), pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value,
                     pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value);
}
static void attach(GCController *controller) {
    recent_buttons = 0;
    selected = controller; controller.handlerQueue = dispatch_get_main_queue();
    __weak GCController *weak_controller = controller;
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *element) {
        (void)element; GCController *current = weak_controller;
        if (current) publish(current, pad);
    };
    fprintf(stderr, "GAMEPAD connected vendor=%s; face buttons follow letters\n", controller.vendorName.UTF8String ?: "Controller");
    publish(controller, controller.extendedGamepad);
}
static void select_controller(void) {
    if (selected) return;
    for (GCController *controller in GCController.controllers) if (controller.extendedGamepad) { attach(controller); break; }
}
void gamepad_active(bool value) {
    active = value;
    if (!value) input_clear();
    else if (selected) publish(selected, selected.extendedGamepad);
}
void gamepad_start(void) {
    GCController.shouldMonitorBackgroundEvents = NO;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    connect_observer = [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *notification) { (void)notification; select_controller(); }];
    disconnect_observer = [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *notification) {
            if (notification.object == selected) {
                selected.extendedGamepad.valueChangedHandler = nil; selected = nil;
                recent_buttons = 0; input_controller_clear(); fprintf(stderr, "GAMEPAD disconnected\n"); select_controller();
            }
        }];
    select_controller();
}
void gamepad_stop(void) {
    selected.extendedGamepad.valueChangedHandler = nil; selected = nil;
    if (connect_observer) [NSNotificationCenter.defaultCenter removeObserver:connect_observer];
    if (disconnect_observer) [NSNotificationCenter.defaultCenter removeObserver:disconnect_observer];
    connect_observer = disconnect_observer = nil; gamepad_active(false);
    recent_buttons = 0;
}
NSString *gamepad_name(void) {
    return selected ? [NSString stringWithFormat:@"%@ · %@", selected.vendorName ?: @"游戏手柄", active ? @"控制游戏中" : @"游戏输入暂停"]
                    : @"未连接 · 支持蓝牙 / USB Xbox 手柄";
}
NSString *gamepad_readout(void) {
    if (!selected) return @"连接后可在此查看摇杆与按键状态";
    GCExtendedGamepad *p = selected.extendedGamepad;
    static NSString *const names[] = {@"A",@"B",@"X",@"Y",@"L3",@"R3",@"LB",@"RB",@"LT",@"RT",@"Menu",@"View",@"←",@"↑",@"→",@"↓"};
    NSMutableArray *pressed = [NSMutableArray new]; uint64_t mask = buttons(p) | recent_buttons; recent_buttons = 0;
    for (unsigned i = 0; i < 16; i++) if (mask & (1ull << i)) [pressed addObject:names[i]];
    return [NSString stringWithFormat:@"左 %+.2f %+.2f   右 %+.2f %+.2f\n%@",
        p.leftThumbstick.xAxis.value, p.leftThumbstick.yAxis.value, p.rightThumbstick.xAxis.value, p.rightThumbstick.yAxis.value,
        pressed.count ? [@"近 1 秒：" stringByAppendingString:[pressed componentsJoinedByString:@" "]] : @"按键已释放"];
}
