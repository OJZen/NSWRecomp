#include "threads.h"
#include <pthread.h>
#include <stdlib.h>

static _Thread_local bool registered;
static _Thread_local uint64_t identifier, mask;
static _Thread_local int priority, core;
static const uint32_t main_handle = 0x8001;

uint32_t threads_init(void) {
    if (registered || pthread_threadid_np(pthread_self(), &identifier)) abort();
    registered = true;
    priority = 44;
    core = 0;
    mask = 7;
    return main_handle;
}

bool threads_destroy(void) { registered = false; return true; }

static bool valid(uint64_t handle) {
    return registered && ((uint32_t)handle == main_handle || (uint32_t)handle == 0xffff8000u);
}

bool threads_svc(unsigned number, uint64_t *r) {
    if (number == 0x10) { if (!registered) return false; r[0] = core < 0 ? __builtin_ctzll(mask) : core; return true; }
    if (number == 0x0c || number == 0x25) {
        bool found = valid(r[1]);
        r[0] = found ? 0 : 0xe401;
        r[1] = found ? (number == 0x0c ? (uint64_t)priority : identifier) : 0;
        return true;
    }
    if (number == 0x0e) {
        bool found = valid(r[2]);
        r[0] = found ? 0 : 0xe401;
        r[1] = found ? (uint32_t)core : 0;
        r[2] = found ? mask : 0;
        return true;
    }
    if (number == 0x0d) {
        uint32_t result = !valid(r[0]) ? 0xe401 : r[1] < 28 || r[1] >= 60 ? 0xe001 : 0;
        if (!result) priority = (int)r[1];
        r[0] = result;
        return true;
    }
    if (number == 0x0f) {
        int next = (int)r[1];
        uint64_t next_mask = r[2];
        uint32_t result = 0;
        if (!valid(r[0])) result = 0xe401;
        else {
            if (next == -2) { next = 0; next_mask = 1; }
            if (next == -3) next = core;
            if (next_mask & ~7ull || next < -1 || next > 2) result = 0xe201;
            else if (!next_mask || (next >= 0 && !(next_mask & (1ull << next)))) result = 0xe801;
            if (!result) { mask = next_mask; core = next; }
        }
        r[0] = result;
        return true;
    }
    // shortcut: only the initial thread exists; reject creation and waits until their full contracts land.
    return false;
}
