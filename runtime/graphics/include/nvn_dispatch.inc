// Compute dispatches share the open copy encoder instead of each opening its own: every encoder is a separate GPU
// kick with fixed start-up cost (v1.4.3 post-cutscene cave: 61 dispatch encoders per frame, 49 of them back to back,
// each a few microseconds of work behind 10-30 us of idle GPU). The encoder barriers on both sides keep the order the
// one-encoder-per-dispatch path had: earlier copies and dispatches finish before it, later ones wait for it.
static id<MTL4ComputeCommandEncoder> zelda_dispatch_encoder(void) {
    id<MTL4ComputeCommandEncoder> encoder = blit();
    zelda_blit_barrier();
    [encoder setArgumentTable:g_ct];
    args_dirty_all();
    return encoder;
}
static void zelda_dispatch_end(void) {
    zelda_blit_barrier();
    args_dirty_all();
}
