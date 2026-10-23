// Real owned-window integration; synthetic pixels, no GPU or game input.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "window.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_uint closes;
static void closed(void *context) { assert(context == &closes); atomic_fetch_add(&closes, 1); }
static const uint8_t colors[4][4] = {{250,30,60,0},{20,230,70,30},{40,80,220,150},{190,120,15,255}};

static void wait_paint(HostWindow *window, uint64_t count) {
    ULONGLONG end = GetTickCount64() + 3000;
    while (host_window_stats(window).painted < count && GetTickCount64() < end) Sleep(10);
    assert(host_window_stats(window).painted >= count);
}
static void verify_pixels(HWND handle, unsigned width, unsigned height) {
    RECT client; assert(GetClientRect(handle, &client));
    LONG fit_width = client.right, fit_height = (LONG)((int64_t)client.right * height / width);
    if (fit_height > client.bottom) { fit_height = client.bottom; fit_width = (LONG)((int64_t)fit_height * width / height); }
    HDC dc = GetDC(handle); assert(dc);
    for (unsigned i = 0; i < 4; i++) {
        int x = (client.right - fit_width) / 2 + fit_width * (i % 2 ? 3 : 1) / 4;
        int y = (client.bottom - fit_height) / 2 + fit_height * (i / 2 ? 3 : 1) / 4;
        assert(GetPixel(dc, x, y) == RGB(colors[i][0], colors[i][1], colors[i][2]));
    }
    if (fit_height < client.bottom - 4) assert(GetPixel(dc, client.right / 2, 1) == RGB(0,0,0));
    if (fit_width < client.right - 4) assert(GetPixel(dc, 1, client.bottom / 2) == RGB(0,0,0));
    assert(ReleaseDC(handle, dc));
}

int main(void) {
    char error[256]; HostWindow *window = NULL;
    assert(!host_window_open(&window, 4097, 1, closed, &closes, error) && !window);
    assert(host_window_open(&window, 320, 240, closed, &closes, error));
    HWND handle = (HWND)host_window_stats(window).native_handle;
    assert(handle && IsWindow(handle) && IsWindowVisible(handle));
    uint8_t pixels[128 * 64 * 4];
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 128; x++)
        memcpy(pixels + (y * 128 + x) * 4, colors[(y >= 32) * 2 + (x >= 64)], 4);
    assert(!host_window_frame(window, pixels, sizeof pixels - 1, 128, 64, error));
    assert(!host_window_frame(window, pixels, sizeof pixels, 0, 64, error));
    assert(host_window_frame(window, pixels, sizeof pixels, 128, 64, error));
    wait_paint(window, 1); verify_pixels(handle, 128, 64);
    // A resize must keep orientation/channel order and center the aspect ratio.
    assert(SetWindowPos(handle, NULL, 40, 40, 450, 220, SWP_NOZORDER));
    assert(host_window_frame(window, pixels, sizeof pixels, 128, 64, error));
    wait_paint(window, 2); verify_pixels(handle, 128, 64);
    ShowWindow(handle, SW_MINIMIZE);
    assert(host_window_frame(window, pixels, sizeof pixels, 128, 64, error));
    ShowWindow(handle, SW_RESTORE);
    wait_paint(window, 3); verify_pixels(handle, 128, 64);
    // UI receives its own copy, independent of the renderer's readback lifetime.
    memset(pixels, 0, sizeof pixels);
    assert(InvalidateRect(handle, NULL, FALSE)); assert(UpdateWindow(handle));
    verify_pixels(handle, 128, 64);
    assert(host_window_stats(window).submitted == 3);
    assert(PostMessageW(handle, WM_CLOSE, 0, 0));
    ULONGLONG end = GetTickCount64() + 3000;
    while (!atomic_load(&closes) && GetTickCount64() < end) Sleep(10);
    assert(atomic_load(&closes) == 1 && host_window_stats(window).closed);
    assert(host_window_frame(window, pixels, sizeof pixels, 128, 64, error));
    assert(host_window_stats(window).submitted == 3);
    assert(host_window_close(window) && !IsWindow(handle));
    assert(host_window_open(&window, 160, 120, closed, &closes, error));
    assert(host_window_close(window) && atomic_load(&closes) == 1);
    puts("Synthetic window pixels, resize, lifetime and close checks passed");
    return 0;
}
