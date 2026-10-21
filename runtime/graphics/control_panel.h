#pragma once
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
@interface TotkControlPanel : NSWindowController
- (instancetype)initWithGameWindow:(NSWindow *)window layer:(CAMetalLayer *)layer;
- (void)showPanel:(id)sender;
@end
