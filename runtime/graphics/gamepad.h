#pragma once
#import <Foundation/Foundation.h>
#include <stdbool.h>
void gamepad_start(void);
void gamepad_stop(void);
void gamepad_active(bool active);
NSString *gamepad_name(void);
NSString *gamepad_readout(void);
