#define main keyboard_checks
#include "native_input.c"
#undef main
#include "../runtime/graphics/gamepad.m"

int main(void) {
    @autoreleasepool {
        keyboard_checks(1, NULL);
        Cpu c = {.x = {1}};
        hle__ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE(&c);
        GCController *device = [GCController controllerWithExtendedGamepad];
        assert(device.isSnapshot && device.extendedGamepad);
        attach(device); gamepad_start(); gamepad_active(true);
        GCExtendedGamepad *pad = device.extendedGamepad;
        GCControllerButtonInput *face[] = {pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY};
        for (unsigned i = 0; i < 4; i++) {
            [face[i] setValue:1]; pad.valueChangedHandler(pad, face[i]);
            [face[i] setValue:0]; pad.valueChangedHandler(pad, face[i]);
            sample(0); assert(memory_words[1] == (1ull << i)); sample(0); assert(!memory_words[1]);
        }
        assert([gamepad_readout() containsString:@"A B X Y"]);
        assert([gamepad_readout() containsString:@"已释放"]);
        [pad.leftThumbstick setValueForXAxis:.8 yAxis:-.6];
        [pad.rightThumbstick setValueForXAxis:-1 yAxis:0];
        [pad.buttonMenu setValue:1]; pad.valueChangedHandler(pad, pad.buttonMenu);
        sample(0); assert(memory_words[1] & (1 << 10));
        assert((int32_t)memory_words[2] > 25000 && (int32_t)memory_words[3] == -32767);
        gamepad_active(false); sample(0); assert(!memory_words[1] && !memory_words[2] && !memory_words[3]);
        pad.valueChangedHandler(pad, pad.buttonMenu); sample(0); assert(!memory_words[1]);
        assert([gamepad_name() containsString:@"暂停"] && [gamepad_readout() containsString:@"Menu"]);
        gamepad_active(true); sample(0); assert(memory_words[1] & (1 << 10));
        input_key(7, 1);
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidDisconnectNotification object:device];
        assert(device.extendedGamepad.valueChangedHandler == nil);
        sample(0); assert(memory_words[1] == 1 && !memory_words[2] && !memory_words[3]);
        gamepad_stop(); sample(0); assert(!memory_words[1]);
    }
    return 0;
}
