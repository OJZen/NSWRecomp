#include "audio.h"
#include "audio_voice.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

static uint8_t pcm[960];
static atomic_uint signals;
static unsigned owned_handles;
bool kernel_event_create(uint32_t *write,uint32_t *read) {*write=1; *read=2; owned_handles=2; return true;}
uint32_t kernel_event_copy(uint32_t read) {assert(read==2); return 3;}
bool kernel_svc(unsigned number,uint64_t *r) {
    if(number==0x11) {assert(r[0]==1); atomic_fetch_add(&signals,1);}
    else {assert(number==0x16 && (r[0]==1 || r[0]==2) && owned_handles); owned_handles--;}
    r[0]=0; return true;
}
bool kernel_access(uint64_t address,size_t size,unsigned permissions) {return permissions==1 && address==(uintptr_t)pcm && size==sizeof pcm;}
static void put32(void *p,uint32_t value) {memcpy(p,&value,4);}
int main(int argc,char **argv) {
    assert(argc==2); unsigned revision=(unsigned)atoi(argv[1]); assert(revision==5 || revision==9 || revision==15);
    uint32_t config[13]={48000,240,2,0,1,0,0,0,0,0,0,0,0x564552|((revision+'0')<<24)};
    assert(audio_open(config,sizeof config)); assert(!audio_open(config,sizeof config)); assert(audio_muted() && audio_event()==3);
    assert(audio_value(0)==48000 && audio_value(1)==240 && audio_value(3)==1);
    unsigned stride=revision==15?0x188:0x170;
    uint32_t h[16]={config[12],16,4*32,stride,0x70,0,0,0,16}; size_t total=64;
    for(unsigned i=1;i<15;i++) total+=h[i]; h[15]=(uint32_t)total;
    uint8_t input[1024]={0},output[4096],canonical[0x188]={0}; memcpy(input,h,64);
    uint64_t address=(uintptr_t)pcm,size=sizeof pcm; memcpy(input+80,&address,8); memcpy(input+88,&size,8); put32(input+96,4);
    canonical[8]=canonical[9]=1; canonical[11]=2; put32(canonical+12,48000); put32(canonical+24,1);
    float pitch=1; memcpy(canonical+28,&pitch,4); put32(canonical+0x54,1);
    audio_wave waves[4]={{.address=(uintptr_t)pcm,.size=sizeof pcm,.end=480,.loops=-1},{.sent=1},{.sent=1},{.sent=1}};
    memcpy(canonical+0x78,waves,sizeof waves); uint8_t *voice=input+80+128+0x70;
    if(revision==15) memcpy(voice,canonical,sizeof canonical);
    else {memcpy(voice,canonical,36); memcpy(voice+0x3c,canonical+0x54,0x170-0x3c);}
    assert(!audio_update(input,total-1,output,sizeof output));
    size_t written=audio_update(input,total,output,sizeof output); assert(written && audio_u32(output)==config[12]);
    assert(!audio_u32(output+128+8)); assert(audio_set(5,0));
    const struct timespec pause={.tv_nsec=35000000}; nanosleep(&pause,NULL); assert(atomic_load(&signals)>0);
    voice[8]=0; voice[(revision==15?0x78:0x60)+26]=1; // The guest marks the submitted wave sent.
    written=audio_update(input,total,output,sizeof output); assert(written && audio_u32(output+128+8)==1);
    uint64_t played; memcpy(&played,output+128,8); assert(played==480);
    assert(audio_set(6,0)); assert(audio_value(3)==1); written=audio_update(input,total,output,sizeof output);
    uint64_t before,after; memcpy(&before,output+written-16,8); nanosleep(&pause,NULL);
    assert(audio_update(input,total,output,sizeof output)==written); memcpy(&after,output+written-16,8); assert(before==after);
    assert(!audio_set(8,101) && audio_set(8,99) && audio_value(9)==99);
    audio_close(); audio_close(); assert(!owned_handles && !audio_event() && !audio_update(input,total,output,sizeof output));
    return 0;
}
