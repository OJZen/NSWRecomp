#pragma once
#include <stdbool.h>
#include "window.h"
bool vulkan_display_native(const void *token);
void vulkan_display_shutdown(void);
bool vulkan_display_window_requested(void);
// Completed GPU readbacks only. An absent/closing preview drops the frame.
bool vulkan_display_frame(const void *rgba, size_t bytes, uint32_t width, uint32_t height,
                          bool *queued, char error[256]);
// Fault/timeout reporting never waits for the presenter lock. A busy presenter
// returns the last snapshot; counters then describe a lower bound.
HostWindowStats vulkan_display_window_stats(void);
