#pragma once
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <AppKit/AppKit.h>
#endif
#import <Metal/Metal.h>
#include <stdbool.h>
#include "shader_status.h"
// A verified offline Metal IR library, or nil so the caller can compile the original source.
id<MTLLibrary> shader_ir_load(id<MTLDevice> device, NSString *source_path, const void *source, size_t length, bool layered);
void shader_ir_report(const char *phase);
// Strict binary hit for a default compute descriptor, or nil for the caller's original compilation path.
id<MTLComputePipelineState> shader_compute_load(id<MTLDevice> device, NSString *source_path, id<MTLFunction> function);
NSData *shader_cache_read(NSString *path, size_t limit);
NSDictionary *shader_render_context(id<MTLDevice> device, NSString *directory, NSString *contract);
BOOL shader_render_capture(id<MTLDevice> device, NSArray<MTLRenderPipelineDescriptor *> *descriptors,
                           NSArray<NSString *> *keys, NSString *directory, NSString *contract, NSString *output, NSError **error);
id<MTLRenderPipelineState> shader_render_load(id<MTLDevice> device, MTLRenderPipelineDescriptor *descriptor,
                                             NSString *key, NSString *directory, NSString *contract);
void shader_render_report(void);
// Compile source libraries before the guest starts, using the runtime's exact transforms/options.
unsigned shader_prep_run(id<MTLDevice> device, NSString *directory, void (^progress)(unsigned, unsigned));
// Startup screen model. shader_screen_state names the current stage from the shared status: library preparation, then
// (when warm says a warm-up will run) the game's boot and the pipeline warm-up at its first submission; runs_at_boot is
// the status pass counter when the game was started. shader_screen_advance turns stages into one forward-only bar.
typedef enum {
    SCREEN_CHECKING, SCREEN_COMPILING, SCREEN_STARTING, SCREEN_READING, SCREEN_LOADING, SCREEN_DESCRIBING,
    SCREEN_BUILDING, SCREEN_READY
} ShaderScreenStage;
typedef struct { ShaderScreenStage stage; unsigned completed, total; } ShaderScreenState;
ShaderScreenState shader_screen_state(bool prep_done, bool warm, unsigned runs_at_boot, ZeldaShaderStatus status);
typedef struct { bool warm; int stage; double base, span, since, fraction; } ShaderScreenBar;
double shader_screen_advance(ShaderScreenBar *bar, ShaderScreenState state, double now);
#if !TARGET_OS_IPHONE
// Shows the startup screen, prepares the libraries, calls done to start the game, and keeps the screen until the
// warm-up has finished.
void shader_prep_begin(NSView *view, id<MTLDevice> device, dispatch_block_t done);
#endif
