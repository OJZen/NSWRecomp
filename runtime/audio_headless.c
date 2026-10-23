// Headless AudioRenderer timing for graphics bring-up. Reuses the Apache-2.0
// voice cursor from Zelda-Mobile (see SOURCE.json); does not mix, decode, apply
// DSP effects or open an output device. Voice ownership and events still advance.
#include "audio.h"
#include "audio_voice.h"
#include "kernel.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t audio_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static atomic_bool running;
static bool started;
static uint32_t config[13], event_write, event_read, time_limit=100;
static uint64_t frames;
static audio_voice *voices;
static uint64_t now_ns(void) {struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) abort(); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void *clock_main(void *unused) {
    (void)unused; uint64_t last=now_ns(); const struct timespec pause={.tv_nsec=5000000};
    while(atomic_load(&running)) {
        nanosleep(&pause,NULL); uint64_t now=now_ns(), steps=(now-last)/5000000; last+=steps*5000000;
        pthread_mutex_lock(&audio_lock); bool signal=started && steps;
        if(signal) {if(steps>4) steps=4; frames+=steps; for(unsigned i=0;i<config[4];i++) audio_advance(&voices[i],frames,config[0]);}
        pthread_mutex_unlock(&audio_lock);
        if(signal) {uint64_t r[31]={event_write}; if(!kernel_svc(0x11,r) || r[0]) abort();}
    }
    return NULL;
}
void audio_close(void) {
    if(!atomic_exchange(&running,false)) return;
    pthread_join(audio_thread,NULL); pthread_mutex_lock(&audio_lock);
    free(voices); voices=NULL; started=false;
    fprintf(stderr,"AUDIO headless timing closed frames=%llu output_samples=0\n",(unsigned long long)frames);
    pthread_mutex_unlock(&audio_lock);
    uint64_t r[31]={event_read}; kernel_svc(0x16,r); r[0]=event_write; kernel_svc(0x16,r); event_read=event_write=0;
}
bool audio_open(const void *parameters,size_t size) {
    if(!parameters || size<sizeof config || atomic_load(&running)) return false;
    uint32_t c[13]; memcpy(c,parameters,sizeof c); unsigned revision=(c[12]>>24)-'0';
    if((c[0]!=48000 && c[0]!=32000) || c[1]!=c[0]/200 || c[2]>1536 || c[3]>=64 || !c[4] || c[4]>1024 || c[5]>256 || c[6]>256 ||
        c[9]>1024 || c[10]>4096 || (c[12]&0xffffff)!=0x564552 || revision<1 || revision>15) return false;
    voices=calloc(c[4],sizeof *voices); if(!voices) return false;
    if(!kernel_event_create(&event_write,&event_read)) {free(voices); voices=NULL; return false;}
    memcpy(config,c,sizeof config); frames=0; started=false; atomic_store(&running,true);
    if(pthread_create(&audio_thread,NULL,clock_main,NULL)) {
        atomic_store(&running,false); free(voices); voices=NULL;
        uint64_t r[31]={event_read}; kernel_svc(0x16,r); r[0]=event_write; kernel_svc(0x16,r); return false;
    }
    if(atexit(audio_close)) {audio_close(); return false;}
    fprintf(stderr,"AUDIO headless timing initialized rate=%u voices=%u revision=%u; no mixing or playback\n",c[0],c[4],revision); return true;
}
uint32_t audio_event(void) {return atomic_load(&running)?kernel_event_copy(event_read):0;}
uint32_t audio_value(unsigned command) {
    pthread_mutex_lock(&audio_lock); uint32_t value=command<=2?config[command]:command==3?!started:command==9?time_limit:UINT32_MAX;
    pthread_mutex_unlock(&audio_lock); return value;
}
bool audio_set(unsigned command,uint32_t value) {
    pthread_mutex_lock(&audio_lock); bool ok=atomic_load(&running);
    if(command==5 || command==6) started=ok && command==5;
    else if(command==8 && value<=100) time_limit=value;
    else ok=false;
    pthread_mutex_unlock(&audio_lock); return ok;
}
void audio_volume(float value) {(void)value;} // No host sound is produced.
void audio_mute(bool value) {(void)value;}
bool audio_muted(void) {return true;}

static bool wave_ranges(const audio_voice *voice,const uint8_t *pool_data,unsigned pool_count) {
    for(unsigned i=0;i<4;i++) if(voice->ready[i]) {
        const audio_wave *wave=&voice->waves[i]; bool found=false;
        for(unsigned j=0;j<pool_count;j++) {
            uint64_t address,size; uint32_t state; memcpy(&address,pool_data+j*32,8); memcpy(&size,pool_data+j*32+8,8); memcpy(&state,pool_data+j*32+16,4);
            if(state>=4 && state<=5 && wave->address>=address && wave->size<=size && wave->address-address<=size-wave->size) found=true;
        }
        if(!found || !kernel_access(wave->address,wave->size,1)) return false;
    }
    return true;
}
size_t audio_update(const void *input,size_t length,void *output,size_t capacity) {
    if(!input || !output || length<64 || length>(4u<<20) || !atomic_load(&running)) return 0;
    uint32_t h[16]; memcpy(h,input,64); size_t total=64;
    for(unsigned i=1;i<15;i++) {if(h[i]>length-total) return 0; total+=h[i];}
    unsigned revision=(config[12]>>24)-'0', voice_size=revision>=15?0x188:0x170, npools=config[4]*4+config[6];
    if(total!=h[15] || h[0]!=config[12] || h[1]!=16 || h[2]!=npools*32 || h[3]!=config[4]*voice_size || h[4]!=config[4]*0x70 ||
        h[5]!=config[6]*0xc0 || h[7]!=config[5]*0x140 || h[8]!=16) {
        fprintf(stderr,"AUDIO unsupported headless update revision=%x sections=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9]); return 0;
    }
    uint32_t out[16]={h[0],0xb0,npools*16,config[4]*16,0,config[6]*(revision>=9?0x90:16),0,config[5]*32,16,0,16};
    size_t written=64; for(unsigned i=1;i<15;i++) written+=out[i]; if(written>capacity) return 0; out[15]=(uint32_t)written;
    const uint8_t *bytes=input, *pool_data=bytes+64+h[1], *voice_data=pool_data+h[2]+h[4], *effects=voice_data+h[3];
    for(unsigned i=0;i<npools;i++) if(audio_u32(pool_data+i*32+16)>6) return 0;
    pthread_mutex_lock(&audio_lock); audio_voice *candidate=malloc(config[4]*sizeof *candidate);
    if(!candidate) {pthread_mutex_unlock(&audio_lock); return 0;} memcpy(candidate,voices,config[4]*sizeof *candidate);
    for(unsigned i=0;i<config[4];i++) {
        uint8_t converted[0x188]={0}; const uint8_t *v=voice_data+i*voice_size;
        if(revision<15) {memcpy(converted,v,36); memcpy(converted+0x54,v+0x3c,0x170-0x3c); v=converted;}
        if(!audio_voice_update(&candidate[i],v,frames,config[0]) || !wave_ranges(&candidate[i],pool_data,npools)) {free(candidate); pthread_mutex_unlock(&audio_lock); return 0;}
    }
    memcpy(voices,candidate,config[4]*sizeof *voices); free(candidate);
    uint8_t *result=output; memset(result,0,written); memcpy(result,out,64);
    for(unsigned i=0;i<npools;i++) {uint32_t state=audio_u32(pool_data+i*32+16); state=state==4?5:state==2?3:0; memcpy(result+64+i*16,&state,4);}
    for(unsigned i=0;i<config[4];i++) {memcpy(result+64+out[2]+i*16,&voices[i].played,8); memcpy(result+64+out[2]+i*16+8,&voices[i].consumed,4);}
    for(unsigned i=0;i<config[6];i++) result[64+out[2]+out[3]+i*(revision>=9?0x90:16)]=effects[i*0xc0] && effects[i*0xc0+2]?3:4;
    memcpy(result+written-16,&frames,8); pthread_mutex_unlock(&audio_lock); return written;
}
