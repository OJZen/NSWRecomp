// Conditional colour clears (SetRenderEnableConditional + ClearColor) are drawn as a predicated quad inside the open pass
// on their target, or at the start of the next pass that attaches it, instead of in a pass of their own. Counter writes
// that the predicate would read stay pending when they are timestamp reports: their counter word is known to become 0.
// Pending counter writes are encoded only before something that may read or overwrite them (nvn_query_gpu.inc).
@class NvProg;
enum { Z_COLOR_CLEAR_MAX = 8 };
typedef struct {
    int index;
    float rgba[4];
    uint32_t mode, known;
    uint64_t predicate, query;   // guest address of the two counters; its GPU address
    long x, y, right, bottom, width, height;
} zelda_color_clear_t;
static struct { int count; zelda_color_clear_t item[Z_COLOR_CLEAR_MAX]; } z_color_clear;
static uint64_t z_color_clear_joined, z_color_clear_deferred, z_color_clear_flushed, z_color_clear_alone, z_query_read_flushes;
static uint64_t z_partial_clear_inline;
static int z_query_n;
static unsigned zelda_color_clear_targets(void) {
    unsigned targets = 0;
    for (int i = 0; i < z_color_clear.count; i++) targets |= 1u << z_color_clear.item[i].index;
    return targets;
}
static void zelda_color_clear_encode(void);
static void zelda_color_clear_unfit(RPD *pass);
static int zelda_color_clear_join(zelda_color_clear_t *c);
static int zelda_color_clear_passthrough(unsigned op, const void *data);
static int zelda_color_clear_touches(uint64_t address, uint64_t length);
static int zelda_inline_partial_clear(const p_clear_color *clear);
static void zelda_query_prepare_draw(NvProg *program, const p_draw_idx *indices);
static int zelda_query_passthrough(unsigned op, const void *data);
static void zelda_query_flush(void);
