#pragma once
#include <stdbool.h>
#include <stdint.h>
// Critical-path trace: begin/end on the CLOCK_UPTIME_RAW base shared with Metal GPU times and performance.tsv.
enum { TL_SUBMIT = 1, TL_COMMIT, TL_GPU, TL_SYNC_WAIT, TL_FENCE, TL_ACQUIRE, TL_PRESENT, TL_SVC_WAIT, TL_BARRIER };
typedef struct { uint64_t begin, end, a, b; uint32_t kind, thread; } ZeldaTimelineEvent;
uint64_t zelda_timeline_now(void); // 0 while not recording, so callers skip clock reads.
void zelda_timeline(unsigned kind, uint64_t begin, uint64_t a, uint64_t b);
void zelda_timeline_span(unsigned kind, uint64_t begin, uint64_t end, uint64_t a, uint64_t b);
bool zelda_timeline_start(void);
bool zelda_timeline_stop(const char *path);
// Script-requested DM_NVN_PASSDUMP window: the backend starts it at the next frame it checks.
void zelda_passdump_request(unsigned frames);
unsigned zelda_passdump_take(void);
// Diagnostic A/B switch, consumed by the render thread after draining its earlier work.
bool zelda_conditional_mode_request(unsigned mode);
int zelda_conditional_mode_take(void);
