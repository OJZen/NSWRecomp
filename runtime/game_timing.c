// Frame timing adapted from Zelda-Mobile tools/game_timing.py (Apache-2.0).
// Verified sites are in config/totk-1.4.3-hooks.json; no global guest clock scaling.
#include "cpu.h"
#include "simd.h"
#include "game_timing.h"
#include <stdatomic.h>
#include <math.h>

static atomic_uint target = 30;
static atomic_bool movie;
static _Atomic double seconds;
static atomic_uint_fast64_t frames;
static double lead; // Only the guest frame-step thread writes this.

bool timing_set_target(unsigned fps) {
    if (fps != 30 && fps != 60 && fps != 90 && fps != 120) return false;
    atomic_store_explicit(&target, fps, memory_order_relaxed);
    return true;
}
unsigned timing_target(void) { return atomic_load_explicit(&target, memory_order_relaxed); }
unsigned zelda_game_frame_rate(void) { return atomic_load_explicit(&movie, memory_order_relaxed) ? 30 : timing_target(); }
double zelda_game_seconds(void) { return atomic_load_explicit(&seconds, memory_order_relaxed); }
uint64_t timing_frames(void) { return atomic_load_explicit(&frames, memory_order_relaxed); }

int rt_hook(Cpu *c, uint64_t site) {
    switch (site - 0x7100100000ull) {
    case 0x2db18c: {
        double elapsed = (double)(int64_t)LD64(c->x[1] + 0xd8) / 19200000.0;
        double step = fmin(fmax(elapsed - lead, 1.0 / zelda_game_frame_rate()), 1.0 / 15.0);
        // Repay short-frame lead on later frames instead of gradually speeding up the game.
        lead = fmin(fmax(lead + step - elapsed, 0), .25);
        float fps = (float)(1.0 / step); uint32_t bits; memcpy(&bits, &fps, sizeof bits);
        ST32(c->x[0] + 0x50, (uint32_t)fps);
        ST32(c->x[0] + 0x24, bits);
        atomic_store_explicit(&seconds, zelda_game_seconds() + step, memory_order_relaxed);
        atomic_fetch_add_explicit(&frames, 1, memory_order_relaxed);
        return 2; // Original epilogue restores the guest's stack and saved registers.
    }
    case 0x2daa54: c->v[0] = LD32(c->x[19] + 0x224); return 1;
    case 0x3e562c: c->v[0] = LD32(c->x[22] + 0x224); return 1;
    case 0x2daa6c: case 0x3e5638: return 1; // Already a float, not an integer conversion.
    case 0xbe14a8: c->x[1] = 1; return 1; // Allow three original-size swapchain images.
    case 0x2b36100: atomic_store_explicit(&movie, true, memory_order_relaxed); return 0;
    case 0x2b33918: atomic_store_explicit(&movie, false, memory_order_relaxed); return 0;
    case 0x1fbc80:
        if ((cmp_s(c, VSF32(1), 0, 0) & 0x40000000) && LD16(c->x[24] - 6) == 5 && LD16(c->x[24] - 8) == 5)
            c->v[8] = 0x3f800000u;
        c->nzcv = cmp_s(c, VSF32(0), 0, 0); // Original FCMP S0, #0.0; S8 is only the FMA multiplier.
        return 1;
    default: rt_udf(c, site);
    }
}
