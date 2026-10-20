// A conditional clear can share the following draw's pass; barriers/copies flush it first.
static struct {
    int pending, has_scissor;
    p_clear_ds clear;
    p_scissor scissor;
    uint64_t parameters, query;
} z_depth_clear;
static void zelda_depth_clear_encode(void);
