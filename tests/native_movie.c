// Adapted from Zelda-Mobile tests/test_movie.c (Apache-2.0).
// Movie decoder HLE driven the way v1.4.3 drives movie::Decoder (host/movie.c header): SDK events, threads,
// Buffer and MediaData are host stand-ins with the SDK's observable behaviour; frames are lossless VP9.
#include "../runtime/graphics/movie.c"
#include <assert.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <vorbis/vorbisenc.h>
#include <math.h>

enum { W = 64, H = 48, FRAMES = 5 };
static unsigned char uploaded_rgba[W*H*4];
static unsigned uploaded_frames;
static uint64_t texture_token = 0x12345000; // Opaque contents must never be dereferenced by the movie bridge.
bool movie_write_rgba(void *texture, const uint8_t *rgba, unsigned width, unsigned height) {
    assert(texture == &texture_token && width == W && height == H);
    memcpy(uploaded_rgba,rgba,sizeof uploaded_rgba); uploaded_frames++; return true;
}
static int prepared_bytes;
void dm_guest_write_prepare(void *address, size_t length) { (void)address; prepared_bytes += (int)length; }

// nn::os events: counted signals the test waits on, like the game's worker WaitAny.
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; int count; } event;
static event events[4];
static void signal_event_stub(void *e) { event *v = e; pthread_mutex_lock(&v->lock); v->count++; pthread_cond_broadcast(&v->cond); pthread_mutex_unlock(&v->lock); }
static void wait_event(event *v) {
    pthread_mutex_lock(&v->lock);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 5;
    while (!v->count) assert(pthread_cond_timedwait(&v->cond, &v->lock, &until) == 0);
    v->count = 0; pthread_mutex_unlock(&v->lock);
}
typedef struct { pthread_t thread; uint64_t entry; void *argument; } thread_type;
static void *thread_main(void *t) { Cpu c = {0}; c.pc = ((thread_type *)t)->entry; c.x[0] = (uintptr_t)((thread_type *)t)->argument; c.sp = 0x7b00000000; rt_dispatch(&c); return NULL; }
static uint32_t create_thread(void *t, uint64_t entry, void *argument, void *stack, size_t size, int priority) {
    assert(stack && !((uintptr_t)stack & 0xfff) && size >= 0x10000 && priority == 16);
    ((thread_type *)t)->entry = entry; ((thread_type *)t)->argument = argument; return 0;
}
static void start_thread(void *t) { assert(!pthread_create(&((thread_type *)t)->thread, NULL, thread_main, t)); }
static void wait_thread(void *t) { pthread_join(((thread_type *)t)->thread, NULL); }
static void destroy_thread(void *t) { (void)t; }
static int released, sdk_allocator_set, guest_allocations;
static void sdk_set_allocator(void *a, void *f, void *r, void *u) { (void)a; (void)f; (void)r; assert(u == (void *)0x5a); sdk_allocator_set++; }
static void *movie_alloc(size_t size, size_t alignment, void *user) {
    assert(user == (void *)0x5a); guest_allocations++;
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return aligned_alloc(alignment, size);
#endif
}
static void movie_free(void *pointer, void *user) {
    assert(user == (void *)0x5a); guest_allocations--;
#ifdef _WIN32
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}
static void release(void *object) { released++; free(object); }
// movie::Buffer accessors observed in subsdk0: offset +0, size +4, capacity +0x18, base +0x28.
typedef struct { int32_t offset, size; uint8_t pad[0x10]; size_t capacity; uint8_t pad2[8]; void *base; } buffer;
static void set_data(void *b, void *data, size_t capacity) { ((buffer *)b)->base = data; ((buffer *)b)->capacity = capacity; }
static void set_range(void *b, int offset, int size) { ((buffer *)b)->offset = offset; ((buffer *)b)->size = size; }
static void *base(const void *b) { return ((const buffer *)b)->base; }
static size_t capacity(const void *b) { return ((const buffer *)b)->capacity; }
static int offset(const void *b) { return ((const buffer *)b)->offset; }
static int buffer_size(const void *b) { return ((const buffer *)b)->size; }
typedef struct { char keys[8][16]; int values[8], count, rect[4]; char mime[32]; buffer *headers[2]; } media;
static unsigned find_int32(const void *m, const char *key, int *value) {
    const media *d = m;
    for (int i = 0; i < d->count; i++) if (!strcmp(d->keys[i], key)) { *value = d->values[i]; return 0xff01; } // only bit 0 is defined
    return 0xff00;
}
static void set_int32(void *m, const char *key, int value) {
    media *d = m; int i = 0;
    while (i < d->count && strcmp(d->keys[i], key)) i++;
    if (i == d->count) { assert(d->count < 8); strcpy(d->keys[d->count++], key); }
    d->values[i] = value;
}
static void set_rect(void *m, const char *key, int l, int t, int r, int b) { assert(!strcmp(key, "crop")); int *x = ((media *)m)->rect; x[0] = l; x[1] = t; x[2] = r; x[3] = b; }
static void set_string(void *m, const char *key, const char *value) { assert(!strcmp(key, "mime")); strcpy(((media *)m)->mime, value); }
static unsigned find_string(const void *m,const char *key,const char **value) {assert(!strcmp(key,"mime")); *value=((const media *)m)->mime; return 1;}
static unsigned find_buffer(const void *m,const char *key,void **value) {
    const media *d=m;
    int i=!strcmp(key,"csd-0")?0:!strcmp(key,"csd-1")?1:-1;
    if(i<0 || !d->headers[i]) return 0xff00;
    *value=d->headers[i]; return 0xff01;
}
uint64_t graphics_guest_export(const char *name) {
    static const struct { const char *name; void *function; } table[] = {
        {"_ZN2nn2os11SignalEventEPNS0_9EventTypeE", signal_event_stub}, {"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi", create_thread},
        {"_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE", start_thread}, {"_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE", wait_thread},
        {"_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE", destroy_thread}, {"_ZdlPv", release},
        {"_ZN2nv2mm12SetAllocatorEPFPvmmS1_EPFvS1_S1_EPFS1_S1_mS1_ES1_", sdk_set_allocator},
        {"_ZN5movie6Buffer18SetDataAndCapacityEPvm", set_data}, {"_ZN5movie6Buffer8SetRangeEii", set_range},
        {"_ZNK5movie6Buffer4BaseEv", base}, {"_ZNK5movie6Buffer8CapacityEv", capacity}, {"_ZNK5movie6Buffer6OffsetEv", offset},
        {"_ZNK5movie9MediaData9FindInt32EPKcPi", find_int32}, {"_ZN5movie9MediaData8SetInt32EPKci", set_int32},
        {"_ZN5movie9MediaData7SetRectEPKciiii", set_rect}, {"_ZN5movie9MediaData9SetStringEPKcS2_", set_string},
        {"_ZNK5movie9MediaData10FindStringEPKcPS2_",find_string},
        {"_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE",find_buffer},
        {"_ZNK5movie6Buffer4SizeEv",buffer_size},
    };
    for (unsigned i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

void die(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); exit(78);
}
bool kernel_access(uint64_t address, size_t size, unsigned permissions) { (void)size; (void)permissions; return address >= 0x10000; }
void threads_checkpoint(void) {}
#define P(i) ((void *)(uintptr_t)c->x[i])
static void sdk_dispatch(Cpu *c) {
    uint64_t a = c->pc;
    if (movie_cpu) assert(c != movie_cpu && c->sp == movie_cpu->sp && !c->x[30] && c->excl_addr == UINT64_MAX);
    if (a == (uintptr_t)signal_event_stub) signal_event_stub(P(0));
    else if (a == (uintptr_t)create_thread) c->x[0] = create_thread(P(0), c->x[1], P(2), P(3), c->x[4], (int)c->x[5]);
    else if (a == (uintptr_t)start_thread) start_thread(P(0));
    else if (a == (uintptr_t)wait_thread) wait_thread(P(0));
    else if (a == (uintptr_t)destroy_thread) destroy_thread(P(0));
    else if (a == (uintptr_t)release) release(P(0));
    else if (a == (uintptr_t)movie_alloc) c->x[0] = (uintptr_t)movie_alloc(c->x[0], c->x[1], P(2));
    else if (a == (uintptr_t)movie_free) movie_free(P(0), P(1));
    else if (a == (uintptr_t)sdk_set_allocator) sdk_set_allocator(P(0), P(1), P(2), P(3));
    else if (a == (uintptr_t)set_data) set_data(P(0), P(1), c->x[2]);
    else if (a == (uintptr_t)set_range) set_range(P(0), (int)c->x[1], (int)c->x[2]);
    else if (a == (uintptr_t)base) c->x[0] = (uintptr_t)base(P(0));
    else if (a == (uintptr_t)capacity) c->x[0] = capacity(P(0));
    else if (a == (uintptr_t)offset) c->x[0] = offset(P(0));
    else if (a == (uintptr_t)find_int32) c->x[0] = find_int32(P(0), P(1), P(2));
    else if (a == (uintptr_t)set_int32) set_int32(P(0), P(1), (int)c->x[2]);
    else if (a == (uintptr_t)set_rect) set_rect(P(0), P(1), (int)c->x[2], (int)c->x[3], (int)c->x[4], (int)c->x[5]);
    else if (a == (uintptr_t)set_string) set_string(P(0), P(1), P(2));
    else if (a == (uintptr_t)find_string) c->x[0]=find_string(P(0),P(1),P(2));
    else if (a == (uintptr_t)find_buffer) c->x[0]=find_buffer(P(0),P(1),P(2));
    else if (a == (uintptr_t)buffer_size) c->x[0]=buffer_size(P(0));
    else abort();
    c->x[19] = 99; c->fpsr |= 1;
}
GuestFn rt_find_function(uint64_t address) { return movie_find_function(address) ? movie_find_function(address) : address > 0x10000 ? sdk_dispatch : NULL; }
void rt_dispatch(Cpu *c) { GuestFn function = rt_find_function(c->pc); assert(function); function(c); }
#undef P

static uint64_t call_slot(unsigned slot, const uint64_t *args, size_t count) {
    Cpu c = {0}; c.sp = 0x7b00000000; c.x[19] = 7; c.x[30] = 0x11223344; c.excl_addr = 123;
    memcpy(c.x, args, count * sizeof *args); c.pc = decoder_vtable[slot];
    assert(movie_find_function(c.pc)); rt_dispatch(&c);
    assert(c.sp == 0x7b00000000 && c.x[19] == 7 && c.x[30] == 0x11223344 && c.excl_addr == 123);
    assert(!movie_cpu);
    return c.x[0];
}
static uint8_t pixel(int frame, int plane, int x, int y) { return (uint8_t)(frame * 29 + plane * 83 + x * 7 + y * 3); }
static size_t packets[FRAMES], packet_offset[FRAMES]; static uint8_t stream[1 << 20];
static void encode(void) {
    vpx_codec_enc_cfg_t config; assert(vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &config, 0) == VPX_CODEC_OK);
    config.g_w = W; config.g_h = H; config.g_timebase.num = 1; config.g_timebase.den = 30; config.g_lag_in_frames = 0;
    config.rc_end_usage = VPX_Q; config.rc_min_quantizer = config.rc_max_quantizer = 0;
    vpx_codec_ctx_t encoder; assert(vpx_codec_enc_init(&encoder, vpx_codec_vp9_cx(), &config, 0) == VPX_CODEC_OK);
    assert(vpx_codec_control(&encoder, VP9E_SET_LOSSLESS, 1) == VPX_CODEC_OK);
    vpx_image_t image; assert(vpx_img_alloc(&image, VPX_IMG_FMT_I420, W, H, 1));
    size_t used = 0;
    for (int frame = 0; frame <= FRAMES; frame++) {
        if (frame < FRAMES)
            for (int plane = 0; plane < 3; plane++)
                for (int y = 0; y < (plane ? H / 2 : H); y++)
                    for (int x = 0; x < (plane ? W / 2 : W); x++) image.planes[plane][y * image.stride[plane] + x] = pixel(frame, plane, x, y);
        assert(vpx_codec_encode(&encoder, frame < FRAMES ? &image : NULL, frame, 1, 0, VPX_DL_GOOD_QUALITY) == VPX_CODEC_OK);
        vpx_codec_iter_t iter = NULL; const vpx_codec_cx_pkt_t *packet;
        while ((packet = vpx_codec_get_cx_data(&encoder, &iter))) if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
            int index = (int)packet->data.frame.pts;
            memcpy(stream + used, packet->data.frame.buf, packet->data.frame.sz);
            packet_offset[index] = used; packets[index] = packet->data.frame.sz; used += packet->data.frame.sz;
        }
    }
    vpx_img_free(&image); vpx_codec_destroy(&encoder);
}
static void check_frame(const uint8_t *nv12, int frame) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) assert(nv12[y * W + x] == pixel(frame, 0, x, y));
    for (int y = 0; y < H / 2; y++) for (int x = 0; x < W / 2; x++) {
        assert(nv12[W * H + y * W + 2 * x] == pixel(frame, 1, x, y));
        assert(nv12[W * H + y * W + 2 * x + 1] == pixel(frame, 2, x, y));
    }
}
#define CALL(slot, ...) call_slot(slot, (uint64_t[]){(uintptr_t)object, ##__VA_ARGS__}, sizeof((uint64_t[]){(uintptr_t)object, ##__VA_ARGS__}) / sizeof(uint64_t))

// Actual six-channel Vorbis, generated from independent sine waves; no external fixture.
enum { AUDIO_SAMPLES=16384, AUDIO_CHANNELS=6, AUDIO_PACKETS=128 };
static double audio_sample(int frame,int channel) {
    const int frequency[]={110,220,330,440,550,30};
    return (0.12+channel*0.02)*sin(2*3.14159265358979323846*frequency[channel]*frame/48000);
}
static void audio_protocol(void) {
    vorbis_info info; vorbis_info_init(&info);
    assert(!vorbis_encode_init_vbr(&info,AUDIO_CHANNELS,48000,0.6f));
    vorbis_comment comment; vorbis_comment_init(&comment);
    vorbis_dsp_state dsp; assert(!vorbis_analysis_init(&dsp,&info));
    vorbis_block block; assert(!vorbis_block_init(&dsp,&block));
    ogg_packet id,comments,setup; assert(!vorbis_analysis_headerout(&dsp,&comment,&id,&comments,&setup));
    // Nonzero offsets test the Buffer range, independently of its full capacity.
    uint8_t *id_copy=calloc(1,(size_t)id.bytes+11), *setup_copy=calloc(1,(size_t)setup.bytes+17);
    assert(id_copy && setup_copy); memcpy(id_copy+3,id.packet,(size_t)id.bytes); memcpy(setup_copy+7,setup.packet,(size_t)setup.bytes);
    buffer headers[2]={{.offset=3,.size=(int)id.bytes,.capacity=(size_t)id.bytes+11,.base=id_copy},
                       {.offset=7,.size=(int)setup.bytes,.capacity=(size_t)setup.bytes+17,.base=setup_copy}};
    assert(!movie_audio_open(id_copy+3,id.bytes,setup_copy+7,setup.bytes,48000,2));
    assert(!movie_audio_open(id_copy+3,4,setup_copy+7,setup.bytes,48000,AUDIO_CHANNELS));
    float **samples=vorbis_analysis_buffer(&dsp,AUDIO_SAMPLES); assert(samples);
    for(int i=0;i<AUDIO_SAMPLES;i++) for(int ch=0;ch<AUDIO_CHANNELS;ch++) samples[ch][i]=(float)audio_sample(i,ch);
    assert(!vorbis_analysis_wrote(&dsp,AUDIO_SAMPLES) && !vorbis_analysis_wrote(&dsp,0));
    uint8_t *packets[AUDIO_PACKETS]; size_t sizes[AUDIO_PACKETS]; unsigned packet_count=0;
    while(vorbis_analysis_blockout(&dsp,&block)==1) {
        assert(!vorbis_analysis(&block,NULL) && !vorbis_bitrate_addblock(&block));
        ogg_packet packet;
        while(vorbis_bitrate_flushpacket(&dsp,&packet)) {
            assert(packet_count<AUDIO_PACKETS && packet.bytes>0);
            sizes[packet_count]=(size_t)packet.bytes; packets[packet_count]=malloc(packet.bytes); assert(packets[packet_count]);
            memcpy(packets[packet_count++],packet.packet,packet.bytes);
        }
    }
    vorbis_block_clear(&block); vorbis_dsp_clear(&dsp); vorbis_comment_clear(&comment); vorbis_info_clear(&info);
    assert(packet_count>4);
    void **object=calloc(1,16); Cpu c={0}; c.x[0]=(uintptr_t)object; c.x[1]=TYPE_VORBIS; c.x[2]=OUTPUT_PCM16; c.x[3]=1; c.x[4]=16;
    hle__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi(&c);
    void *registered[4]={&events[0],&events[1],&events[2],&events[3]}; assert(CALL(3,(uintptr_t)registered)==ST_OK);
    media track={0}; set_int32(&track,"sample-rate",48000); set_int32(&track,"channel-count",AUDIO_CHANNELS); set_string(&track,"mime","audio/vorbis");
    track.headers[0]=&headers[0]; track.headers[1]=&headers[1];
    assert(CALL(2,(uintptr_t)&track)==ST_OK && CALL(5)==ST_OK); wait_event(&events[0]);
    // Replaying after Flush must reset overlap and packet state and yield the same PCM.
    for(unsigned pass=0;pass<2;pass++) {
        unsigned next=0,loops=0,format_seen=0; size_t received=0;
        int16_t *decoded=calloc((AUDIO_SAMPLES+8192)*AUDIO_CHANNELS,sizeof *decoded); assert(decoded);
        for(;;) {
            assert(++loops<5000);
            int index,more; buffer input={0};
            if(next<=packet_count && CALL(9,(uintptr_t)&index,(uintptr_t)&more)==ST_OK) {
                assert(CALL(10,index,(uintptr_t)&input)==ST_OK);
                if(next<packet_count) {memcpy(input.base,packets[next],sizes[next]); assert(CALL(11,index,0,sizes[next],1000+next*20000,0)==ST_OK);}
                else assert(CALL(11,index,0,0,999999,FLAG_EOS)==ST_OK);
                next++;
            }
            int64_t pts; uint32_t flags;
            if(CALL(12,(uintptr_t)&index,(uintptr_t)&pts,(uintptr_t)&flags,(uintptr_t)&more)!=ST_OK) {
                struct timespec interval={0,1000000}; nanosleep(&interval,NULL); continue;
            }
            if(flags==FLAG_EOS) {assert(pts==999999 && CALL(15,index)==ST_OK); break;}
            assert(!flags && pts>=1000 && pts<999999);
            if(!format_seen++) {
                if(!pass) wait_event(&events[2]);
                media format={0}; assert(CALL(4,(uintptr_t)&format)==ST_OK && !strcmp(format.mime,"audio/raw"));
                int value; assert(find_int32(&format,"channel-count",&value)&1 && value==AUDIO_CHANNELS);
                assert(find_int32(&format,"sample-rate",&value)&1 && value==48000);
            }
            buffer output={0}; set_data(&output,decoded+received,((AUDIO_SAMPLES+8192)*AUDIO_CHANNELS-received)*sizeof *decoded);
            assert(CALL(13,index,(uintptr_t)&output)==ST_OK && output.size>0 && output.size%(AUDIO_CHANNELS*2)==0);
            received+=(size_t)output.size/2; assert(CALL(15,index)==ST_OK);
        }
        assert(received>=AUDIO_SAMPLES*AUDIO_CHANNELS && received<(AUDIO_SAMPLES+8192)*AUDIO_CHANNELS);
        for(int ch=0;ch<AUDIO_CHANNELS;ch++) {
            double error=0;
            for(int i=512;i<AUDIO_SAMPLES-512;i++) {double difference=decoded[i*AUDIO_CHANNELS+ch]/32768.0-audio_sample(i,ch); error+=difference*difference;}
            fprintf(stderr,"Vorbis channel=%d samples=%zu mean_square_error=%g\n",ch,received/AUDIO_CHANNELS,error/(AUDIO_SAMPLES-1024));
            // The encoder applies a separate low-pass model to the sixth (LFE) channel.
            assert(error/(AUDIO_SAMPLES-1024)<(ch==5 ? 0.001 : 0.0003));
        }
        free(decoded); assert(CALL(16)==ST_OK && CALL(8)==ST_OK);
    }
    assert(CALL(6)==ST_OK); CALL(1); assert(released==1 && !guest_allocations);
    for(unsigned i=0;i<packet_count;i++) free(packets[i]);
    free(id_copy); free(setup_copy);
    puts("Vorbis movie: six channel PCM, configuration, events, EOS and flush replay passed");
}

int main(int argc, char **argv) {
    bool texture_mode = argc == 2 && !strcmp(argv[1], "texture");
    encode();
    for (int i = 0; i < 4; i++) { pthread_mutex_init(&events[i].lock, NULL); pthread_cond_init(&events[i].cond, NULL); }
    // The game registers its movie allocator first; the SDK still receives it, and the worker stack comes from it.
    Cpu c = {0}; c.x[0] = (uintptr_t)movie_alloc; c.x[1] = (uintptr_t)movie_free; c.x[3] = 0x5a;
    hle__ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(&c);
    assert(sdk_allocator_set == 1);
    if(argc==2 && !strcmp(argv[1],"audio")) {
        audio_protocol();
        for(int i=0;i<4;i++) {pthread_mutex_destroy(&events[i].lock); pthread_cond_destroy(&events[i].cond);}
        return 0;
    }
    void **object = malloc(16);
    c.x[0] = (uintptr_t)object; c.x[1] = TYPE_VP9; c.x[2] = texture_mode ? OUTPUT_TEXTURE : OUTPUT_NV12; c.x[3] = texture_mode ? 2 : 1; c.x[4] = 16;
    hle__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi(&c);
    assert(object[0] == (void *)decoder_vtable);
    assert(!movie_find_function(MOVIE_BASE - 4) && !movie_find_function(MOVIE_BASE + 1) && !movie_find_function(MOVIE_BASE + 80));
    void *registered[4] = {&events[0], &events[1], &events[2], &events[3]};
    assert(CALL(3, (uintptr_t)registered) == ST_OK);
    media track = {0}; set_int32(&track, "width", W); set_int32(&track, "height", H);
    assert(CALL(2, (uintptr_t)&track) == ST_OK);
    int count, size; assert(CALL(14, (uintptr_t)&count, (uintptr_t)&size) == ST_OK && count == OUTPUTS && size == (texture_mode ? W * H * 4 : W * H * 3 / 2));
    int index, more; assert(CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_OK && index == 0 && more == INPUTS - 1);
    assert(CALL(5) == ST_OK && guest_allocations == INPUTS + 2);
    wait_event(&events[0]);
    // Game input thread: take every free input, fill it like Extractor::Read, send it with its sample time.
    int next = 0; uint8_t texture[W * H * 3 / 2];
    buffer input = {0}, output = {0};
    int indices[INPUTS] = {index}, held = 1;
    while (held < INPUTS && CALL(9, (uintptr_t)&indices[held], (uintptr_t)&more) == ST_OK) held++;
    assert(CALL(11, indices[0], INPUT_CAPACITY, 1, 0, 0) == ST_FAILED);
    assert(CALL(11, INPUTS, 0, 0, 0, 0) == ST_FAILED);
    assert(held == INPUTS && CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_TRY_AGAIN);
    for (int i = 0; i < held && next < FRAMES; i++, next++) {
        assert(CALL(10, indices[i], (uintptr_t)&input) == ST_OK && input.capacity == INPUT_CAPACITY);
        memcpy((uint8_t *)input.base + 8, stream + packet_offset[next], packets[next]); set_range(&input, 8, (int)packets[next]);
        assert(CALL(11, indices[i], 8, (int)packets[next], next * 33333LL, 0) == ST_OK);
    }
    int shown = 0, format_seen = 0;
    while (shown < FRAMES) {
        // Game worker: on input availability queue the freed indices and keep feeding; on output take frames.
        if (next < FRAMES && CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_OK) {
            assert(CALL(10, index, (uintptr_t)&input) == ST_OK);
            memcpy(input.base, stream + packet_offset[next], packets[next]);
            assert(CALL(11, index, 0, (int)packets[next], next * 33333LL, 0) == ST_OK);
            next++;
            continue;
        }
        if (next == FRAMES && CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_OK) {
            assert(CALL(11, index, 0, 0, 0LL, FLAG_EOS) == ST_OK);
            next++;
        }
        int64_t pts; uint32_t flags;
        if (CALL(12, (uintptr_t)&index, (uintptr_t)&pts, (uintptr_t)&flags, (uintptr_t)&more) != ST_OK) { wait_event(&events[1]); continue; }
        if (!format_seen) {
            wait_event(&events[2]); media format = {0};
            assert(CALL(4, (uintptr_t)&format) == ST_OK);
            int value; assert(find_int32(&format, "width", &value) & 1 && value == W); assert(find_int32(&format, "stride", &value) & 1 && value == W);
            assert(format.rect[2] == W - 1 && format.rect[3] == H - 1 && !strcmp(format.mime, "video/x-vnd.on2.vp9"));
            format_seen = 1;
        }
        assert(flags == 0 && pts == shown * 33333LL);
        if (texture_mode) {
            set_data(&output,&texture_token,sizeof texture_token); set_range(&output,1,0);
            assert(CALL(13,index,(uintptr_t)&output)==ST_FAILED && uploaded_frames==(unsigned)shown);
            set_range(&output,0,sizeof texture_token);
            assert(CALL(13,index,(uintptr_t)&output)==ST_OK && uploaded_frames==(unsigned)shown+1);
            for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
                int luma=pixel(shown,0,x,y)-16,u=pixel(shown,1,x/2,y/2)-128,v=pixel(shown,2,x/2,y/2)-128;
                int color[3]={(298*luma+409*v+128)>>8,(298*luma-100*u-208*v+128)>>8,(298*luma+516*u+128)>>8};
                for(unsigned ch=0;ch<3;ch++) {int expected=color[ch]<0?0:color[ch]>255?255:color[ch]; assert(abs(uploaded_rgba[(y*W+x)*4+ch]-expected)<=2);}
                assert(uploaded_rgba[(y*W+x)*4+3]==255);
            }
            shown++;
        } else {
        // Renderer: a Buffer over the game's NV12 texture memory, then release.
        set_data(&output, texture, sizeof texture); set_range(&output, 0, sizeof texture);
        int before = prepared_bytes;
        set_range(&output, -1, 0);
        assert(CALL(13, index, (uintptr_t)&output) == ST_FAILED && prepared_bytes == before);
        set_range(&output, sizeof texture + 1, 0);
        assert(CALL(13, index, (uintptr_t)&output) == ST_FAILED && prepared_bytes == before);
        set_range(&output, 0, sizeof texture);
        assert(CALL(13, index, (uintptr_t)&output) == ST_OK && output.size == W * H * 3 / 2);
        assert(prepared_bytes - before == W * H * 3 / 2);
        check_frame(texture, shown++);
        }
        assert(CALL(15, index) == ST_OK && CALL(15, index) == ST_FAILED);
    }
    // End of stream: one more input carries flag 4 and comes back as an empty flag-4 output.
    for (;;) {
        if (next == FRAMES && CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_OK) {
            assert(CALL(11, index, 0, 0, 0LL, FLAG_EOS) == ST_OK);
            next++;
        }
        int64_t pts; uint32_t flags;
        if (CALL(12, (uintptr_t)&index, (uintptr_t)&pts, (uintptr_t)&flags, (uintptr_t)&more) == ST_OK) {
            assert(flags == FLAG_EOS); assert(CALL(15, index) == ST_OK); break;
        }
        wait_event(&events[1]);
    }
    assert(CALL(16) == ST_OK);
    // Skip/destroy path: Flush frees every slot, Stop idles, the deleting destructor joins and frees.
    assert(CALL(8) == ST_OK && CALL(6) == ST_OK);
    assert(CALL(9, (uintptr_t)&index, (uintptr_t)&more) == ST_OK && more == INPUTS - 1);
    CALL(1);
    assert(released == 1 && guest_allocations == 0);
    for (int i = 0; i < 4; i++) { pthread_mutex_destroy(&events[i].lock); pthread_cond_destroy(&events[i].cond); }
    puts("Movie decoder HLE: events, input/output slots, NV12 delivery, format, end of stream and teardown passed");
    return 0;
}
