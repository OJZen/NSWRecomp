#include "game_timing.h"
#include "cpu.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>

void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
static unsigned char parent[0x400], frame[0x100];
static uint64_t count;
static double tick(Cpu *c, uint64_t ticks) {
    ST64((uintptr_t)parent + 0xd8, ticks);
    Cpu before = *c;
    assert(rt_hook(c, 0x71003db18c) == 2);
    assert(!memcmp(c, &before, sizeof *c));
    assert(timing_frames() == ++count);
    float fps; memcpy(&fps, frame + 0x24, 4);
    assert(fps >= 15 && fps <= zelda_game_frame_rate());
    assert(LD32((uintptr_t)frame + 0x50) == (uint32_t)fps);
    return 1.0 / fps;
}
int main(void) {
    Cpu c = {0}; c.x[0] = (uintptr_t)frame; c.x[1] = (uintptr_t)parent;
    assert(timing_target() == 30);
    for (unsigned fps = 30; fps <= 120; fps += 30) {
        assert(timing_set_target(fps));
        double start = zelda_game_seconds(), wall = 0;
        for (unsigned i = 0; i < 10000; i++) {
            uint64_t ticks = (uint64_t)(19200000.0 / fps * (i % 2 ? 1.4 : .6));
            wall += ticks / 19200000.0;
            tick(&c, ticks);
        }
        assert(fabs(zelda_game_seconds() - start - wall) < .034);
        assert(rt_hook(&c, 0x7102c36100) == 0 && zelda_game_frame_rate() == 30);
        assert(timing_target() == fps);
        assert(rt_hook(&c, 0x7102c33918) == 0 && zelda_game_frame_rate() == fps);
    }
    assert(!timing_set_target(0) && !timing_set_target(45) && !timing_set_target(121));
    assert(timing_target() == 120);
    assert(tick(&c, 0) <= 1.0 / 15);
    assert(fabs(tick(&c, 19200000) - 1.0 / 15) < 1e-8);
    c.x[19] = c.x[22] = (uintptr_t)parent;
    ST32((uintptr_t)parent + 0x224, 0x42b40000); // 90.0f
    assert(rt_hook(&c, 0x71003daa54) == 1 && c.v[0] == 0x42b40000);
    assert(rt_hook(&c, 0x71004e562c) == 1 && c.v[0] == 0x42b40000);
    assert(rt_hook(&c, 0x71003daa6c) == 1 && rt_hook(&c, 0x71004e5638) == 1);
    assert(rt_hook(&c, 0x7100ce14a8) == 1 && c.x[1] == 1);
    c.x[24] = (uintptr_t)parent + 16; c.v[0] = c.v[1] = c.v[8] = 0;
    ST16(c.x[24] - 6, 5); ST16(c.x[24] - 8, 5);
    assert(rt_hook(&c, 0x71002fbc80) == 1 && c.v[8] == 0x3f800000);
    assert(c.nzcv == 0x60000000); // Original FCMP S0, #0.0, independent of the initialized S8.
    c.v[8] = 0; ST16(c.x[24] - 6, 4);
    rt_hook(&c, 0x71002fbc80); assert(c.v[8] == 0 && c.nzcv == 0x60000000);
    c.v[1] = 0x7f800001; c.fpsr = 0;
    rt_hook(&c, 0x71002fbc80); assert(c.v[8] == 0 && (c.fpsr & 1));
    const uint32_t values[] = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7fc00001, 0x7f800001};
    const uint32_t flags[] = {0x60000000, 0x60000000, 0x20000000, 0x80000000, 0x30000000, 0x30000000};
    c.v[1] = 0x3f800000; // No transition initialization; the original compare must still run.
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        c.v[0] = values[i]; c.v[8] = 0xbf800000; c.fpsr = 0;
        rt_hook(&c, 0x71002fbc80);
        assert(c.nzcv == flags[i] && c.v[8] == 0xbf800000);
        assert((c.fpsr & 1) == (i == 5));
    }
    return 0;
}
