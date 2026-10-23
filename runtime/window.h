// Bounded Windows preview of completed RGBA8 readbacks. No Vulkan/guest ownership.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HostWindow HostWindow;
typedef struct {
    bool created, closed;
    uint64_t submitted, painted;
    uintptr_t native_handle;
} HostWindowStats;
typedef void (*HostWindowClosed)(void *context);

// Owns a UI thread. A failed open may still return an object to close; if close
// cannot join within its bound, retain it until the bounded process exits.
bool host_window_open(HostWindow **out, uint32_t width, uint32_t height,
                      HostWindowClosed closed, void *context, char error[256]);
// Copies top-down RGBA8 (alpha ignored); max 4096x4096 / 64 MiB per frame.
// A closing window drops new frames. Only accepted copies increment submitted.
bool host_window_frame(HostWindow *, const void *rgba, size_t capacity,
                       uint32_t width, uint32_t height, char error[256]);
HostWindowStats host_window_stats(const HostWindow *);
// Programmatic close suppresses the user-close callback. True frees the object;
// false retains every resource because the UI thread has not stopped yet.
bool host_window_close(HostWindow *);
