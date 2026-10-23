#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "window.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { FRAME_MESSAGE = WM_APP + 1 };
struct HostWindow {
    CRITICAL_SECTION lock;
    HANDLE thread, ready;
    DWORD thread_id;
    atomic_uintptr_t handle;
    atomic_bool created, closing, closed, programmatic, posted, user_closed;
    atomic_uint_fast64_t submitted, painted;
    uint64_t last_painted;
    uint32_t width, height, initial_width, initial_height;
    uint8_t *pixels;
    DWORD failure;
    HostWindowClosed on_close;
    void *context;
};

static bool dimensions(uint32_t width, uint32_t height) {
    return width && height && width <= 4096 && height <= 4096;
}

static void paint(HostWindow *window, HWND handle) {
    PAINTSTRUCT state;
    HDC dc = BeginPaint(handle, &state);
    RECT client; GetClientRect(handle, &client);
    FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    EnterCriticalSection(&window->lock);
    LONG width = client.right, height = client.bottom;
    if (window->pixels && width > 0 && height > 0) {
        LONG fit_width = width, fit_height = (LONG)((int64_t)width * window->height / window->width);
        if (fit_height > height) {
            fit_height = height; fit_width = (LONG)((int64_t)height * window->width / window->height);
        }
        if (fit_width > 0 && fit_height > 0) {
            BITMAPINFO info = {0};
            info.bmiHeader.biSize = sizeof info.bmiHeader;
            info.bmiHeader.biWidth = (LONG)window->width;
            info.bmiHeader.biHeight = -(LONG)window->height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            int rows = StretchDIBits(dc, (width - fit_width) / 2, (height - fit_height) / 2,
                fit_width, fit_height, 0, 0, (int)window->width, (int)window->height,
                window->pixels, &info, DIB_RGB_COLORS, SRCCOPY);
            uint64_t sequence = atomic_load(&window->submitted);
            if (rows > 0 && sequence != window->last_painted) {
                window->last_painted = sequence;
                atomic_fetch_add(&window->painted, 1);
            }
        }
    }
    LeaveCriticalSection(&window->lock);
    EndPaint(handle, &state);
}

static LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    HostWindow *window = (void *)GetWindowLongPtrW(handle, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        window = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (!window) return DefWindowProcW(handle, message, wparam, lparam);
    switch (message) {
        case FRAME_MESSAGE:
            atomic_store(&window->posted, false);
            InvalidateRect(handle, NULL, FALSE); return 0;
        case WM_SIZE:
            InvalidateRect(handle, NULL, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(window, handle); return 0;
        case WM_CLOSE:
            if (!atomic_load(&window->programmatic)) atomic_store(&window->user_closed, true);
            atomic_store(&window->closing, true); DestroyWindow(handle); return 0;
        case WM_DESTROY:
            atomic_store(&window->handle, 0); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(handle, message, wparam, lparam);
    }
}

static DWORD WINAPI ui_thread(void *argument) {
    HostWindow *window = argument;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW type = {.lpfnWndProc = window_proc, .hInstance = instance,
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)), .lpszClassName = L"NSWRecompReadbackPreview"};
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        window->failure = GetLastError(); goto done;
    }
    RECT rectangle = {0, 0, (LONG)window->initial_width, (LONG)window->initial_height};
    if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE)) { window->failure = GetLastError(); goto done; }
    HWND handle = CreateWindowExW(0, type.lpszClassName, L"NSWRecomp - Vulkan preview", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        NULL, NULL, instance, window);
    if (!handle) { window->failure = GetLastError(); goto done; }
    atomic_store(&window->handle, (uintptr_t)handle);
    atomic_store(&window->created, true);
    ShowWindow(handle, SW_SHOWNORMAL);
    SetEvent(window->ready);
    if (atomic_load(&window->closing)) PostMessageW(handle, WM_CLOSE, 0, 0);
    MSG message;
    int result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (result < 0) { window->failure = GetLastError(); DestroyWindow(handle); }
done:
    atomic_store(&window->closing, true);
    atomic_store(&window->closed, true);
    SetEvent(window->ready);
    if (atomic_load(&window->user_closed) && window->on_close)
        window->on_close(window->context);
    return 0;
}

bool host_window_open(HostWindow **out, uint32_t width, uint32_t height,
                      HostWindowClosed closed, void *context, char error[256]) {
    if (!out) { snprintf(error, 256, "missing preview owner"); return false; }
    *out = NULL;
    if (!dimensions(width, height)) { snprintf(error, 256, "preview dimensions exceed bound"); return false; }
    HostWindow *window = calloc(1, sizeof *window);
    if (!window) { snprintf(error, 256, "preview allocation failed"); return false; }
    InitializeCriticalSection(&window->lock);
    window->initial_width = width; window->initial_height = height;
    window->on_close = closed; window->context = context;
    *out = window;
    window->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (window->ready) window->thread = CreateThread(NULL, 0, ui_thread, window, 0, &window->thread_id);
    if (!window->ready || !window->thread) {
        snprintf(error, 256, "preview thread creation failed: %lu", GetLastError()); return false;
    }
    if (WaitForSingleObject(window->ready, 5000) != WAIT_OBJECT_0) {
        snprintf(error, 256, "preview initialization timed out"); return false;
    }
    if (!atomic_load(&window->created) || atomic_load(&window->closed)) {
        snprintf(error, 256, "preview window creation failed: %lu", window->failure); return false;
    }
    return true;
}

bool host_window_frame(HostWindow *window, const void *rgba, size_t capacity,
                       uint32_t width, uint32_t height, char error[256]) {
    if (!window || !rgba || !dimensions(width, height) || capacity < (size_t)width * height * 4) {
        snprintf(error, 256, "invalid preview RGBA frame"); return false;
    }
    if (atomic_load(&window->closing)) return true;
    size_t bytes = (size_t)width * height * 4;
    uint8_t *pixels = malloc(bytes);
    if (!pixels) { snprintf(error, 256, "preview frame allocation failed"); return false; }
    const uint8_t *source = rgba;
    for (size_t i = 0; i < bytes; i += 4) {
        pixels[i] = source[i + 2]; pixels[i + 1] = source[i + 1]; pixels[i + 2] = source[i]; pixels[i + 3] = 255;
    }
    EnterCriticalSection(&window->lock);
    if (atomic_load(&window->closing)) { LeaveCriticalSection(&window->lock); free(pixels); return true; }
    uint8_t *previous = window->pixels; window->pixels = pixels;
    window->width = width; window->height = height;
    atomic_fetch_add(&window->submitted, 1);
    HWND handle = (HWND)atomic_load(&window->handle);
    bool posted = atomic_exchange(&window->posted, true);
    bool ok = posted || PostMessageW(handle, FRAME_MESSAGE, 0, 0);
    LeaveCriticalSection(&window->lock); free(previous);
    if (!ok && !atomic_load(&window->closing)) { snprintf(error, 256, "cannot schedule preview repaint"); return false; }
    return true;
}

HostWindowStats host_window_stats(const HostWindow *window) {
    if (!window) return (HostWindowStats){0};
    return (HostWindowStats){atomic_load(&window->created), atomic_load(&window->closed),
        atomic_load(&window->submitted), atomic_load(&window->painted), atomic_load(&window->handle)};
}

bool host_window_close(HostWindow *window) {
    if (!window) return true;
    atomic_store(&window->programmatic, true); atomic_store(&window->closing, true);
    HWND handle = (HWND)atomic_load(&window->handle);
    if (handle) PostMessageW(handle, WM_CLOSE, 0, 0);
    if (window->thread && (GetCurrentThreadId() == window->thread_id ||
            WaitForSingleObject(window->thread, 5000) != WAIT_OBJECT_0)) return false;
    if (window->thread) CloseHandle(window->thread);
    if (window->ready) CloseHandle(window->ready);
    free(window->pixels); DeleteCriticalSection(&window->lock); free(window);
    return true;
}
