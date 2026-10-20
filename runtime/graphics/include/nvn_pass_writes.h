// Constant-buffer updates waiting for the next render pass (nvn_pass_writes.inc).
static int z_write_n, z_write_keep;
static void zelda_write_encode(void);
static void zelda_write_flush(void);
static int zelda_write_pending(void);
static uint64_t z_write_deferred, z_write_in_pass, z_write_copied, z_write_replaced;
static int zelda_write_defer(uint64_t source, uint64_t destination, uint64_t bytes);
