// Protocol cases adapted from tests/native_services.c; Apache-2.0,
// see third_party/zelda-mobile/LICENSE. Synthetic resources; no game, font or audio inputs.
#include "services.h"
#include "kernel.h"
#include "host.h"
#include "time_clock.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t message[65], result;
static bool send(uint32_t handle) {
    message[64] = 0xdeadbeef;
    bool handled = service_request(handle, message, 256, &result);
    assert(message[64] == 0xdeadbeef);
    return handled;
}
static void cmif(unsigned type, unsigned command) {
    memset(message, 0, sizeof message);
    message[0] = type; message[1] = 8; message[4] = 0x49434653; message[6] = command;
}
static void domain(unsigned object, unsigned command) {
    memset(message, 0, sizeof message);
    message[0] = 6; message[1] = 10; message[4] = 0x100001; message[5] = object;
    message[8] = 0x49434653; message[10] = command;
}
static void get_service(const char *name) {
    memset(message, 0, sizeof message); message[0] = 17; message[1] = 2;
    memcpy(message + 2, name, strlen(name));
}
static uint32_t connect_sm(void) {
    uint32_t handle; assert(service_connect("sm:", &handle)); return handle;
}
static void register_sm(uint32_t handle) {
    memset(message, 0, sizeof message);
    message[0] = 16; message[1] = 0x80000000; message[2] = 1;
    assert(send(handle) && !result && message[1] == 1 && !message[2]);
}
static void *independent_client(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t sm = connect_sm(), r;
        uint32_t request[8] = {16, 0x80000000, 1, 0, 0};
        assert(service_request(sm, request, sizeof request, &r) && !r);
        assert(!service_set_access("*"));
        assert(service_close(sm));
    }
    return NULL;
}
static void logger_frame(uint64_t address, uint32_t length) {
    memset(message, 0, sizeof message);
    message[0] = 6 | (1u << 20); message[1] = 15;
    message[2] = length; message[3] = (uint32_t)address;
    message[4] = (((address >> 32) & 15) << 28) | ((address >> 36) << 2);
    message[8] = 0x100001; message[9] = 2; message[12] = 0x49434653;
}

static void storage_read_frame(uint64_t address, unsigned capacity, uint64_t offset, uint64_t length) {
    memset(message, 0, sizeof message);
    message[0] = 6 | (1u << 24); message[1] = 15;
    message[2] = capacity; message[3] = (uint32_t)address;
    message[4] = 1 | (((address >> 32) & 15) << 28) | ((address >> 36) << 2);
    message[8] = 0x200001; message[9] = 2; message[12] = 0x49434653;
    memcpy(message + 16, &offset, 8); memcpy(message + 18, &length, 8);
}
static void check_storage(uint32_t fs) {
    const char *path = getenv("NSW_TEST_ROMFS"); assert(path);
#ifdef _WIN32
    assert(!_putenv_s("ZELDA_ROMFS", path));
#else
    assert(!setenv("ZELDA_ROMFS", path, 1));
#endif
    cmif(5, 2); assert(send(fs) && !result); uint32_t clone = message[3];
    domain(1, 200); assert(send(clone) && !result && message[12] == 2);
    domain(2, 4); assert(send(fs) && !result && message[12] == 0x20300 && !message[13]);
    uint8_t *bytes = host_reserve_guest(0x40000); assert(bytes && !host_protect(bytes, 0x30000, 3));
    memset(bytes, 0xa5, 0x30000);
    storage_read_frame((uintptr_t)bytes, 0x20000, 0x200, 0x20000); assert(send(clone) && !result);
    for (size_t i = 0; i < 0x20000; i++) assert(bytes[i] == (uint8_t)i);
    assert(bytes[0x20000] == 0xa5);
    storage_read_frame((uintptr_t)bytes, 80, 0x202ff, 2); assert(send(fs) && result == 0x1c01);
    storage_read_frame((uintptr_t)bytes, 79, 0, 80); assert(send(fs) && result == 0x1c01);
    storage_read_frame((uintptr_t)bytes, 80, UINT64_MAX, 1); assert(send(fs) && result == 0x1c01);
    storage_read_frame(1, 80, 0, 80); assert(send(fs) && result == 0xe601);
    assert(!host_protect(bytes, 0x10000, 1));
    storage_read_frame((uintptr_t)bytes, 80, 0, 80); assert(send(fs) && result == 0xe601);
    storage_read_frame(0, 0, 0x20300, 0); assert(send(fs) && !result);
    domain(2, 1); assert(!send(fs)); // Writes remain unsupported.
    domain(2, 0); message[4] = 2; assert(send(clone) && !result);
    domain(2, 4); assert(send(fs) && result == 0x1c01);
    assert(service_close(clone)); assert(!host_release(bytes, 0x40000));
}

static void check_applet(uint32_t sm, uint32_t *waits) {
    get_service("appletOE"); assert(send(sm) && !result); uint32_t applet = message[3];
    cmif(5, 0); assert(send(applet) && !result);
    memset(message, 0, sizeof message);
    message[0] = 6; message[1] = 0x8000000c; message[2] = 3; message[5] = 0xffff8001;
    message[8] = 0x180001; message[9] = 1; message[12] = 0x49434653;
    assert(send(applet) && !result && message[12] == 2);
    domain(2, 0); assert(send(applet) && !result && message[12] == 3);
    domain(2, 1); assert(send(applet) && !result && message[12] == 4);
    domain(2, 2); assert(send(applet) && !result && message[12] == 5);
    domain(5, 10); assert(send(applet) && !result);
    domain(3, 0); assert(send(applet) && !result && message[2] == 2); waits[0] = message[3];
    domain(3, 0); assert(send(applet) && !result && message[2] == 2); waits[1] = message[3];
    assert(waits[0] != waits[1]);
    uint64_t r[31] = {0, (uintptr_t)waits, 1, 0}; assert(kernel_svc(0x18, r) && !r[0]);
    domain(3, 1); assert(send(applet) && !result && message[12] == 15 && !message[10]);
    r[1] = (uintptr_t)(waits + 1); r[2] = 1; r[3] = 0;
    assert(kernel_svc(0x18, r) && r[0] == 0xea01);
    domain(3, 1); assert(send(applet) && !result && message[10] == 0x680);
    domain(3, 66); assert(send(applet) && result == 0x1c01);
    for (unsigned mode = 0; mode <= 3; mode++) {
        domain(3, 66); message[4] = 0x140001; message[1] = 11; message[12] = mode;
        if (mode <= 2) assert(send(applet) && !result);
        else assert(!send(applet) && !strcmp(service_blocker(), "invalid CPU boost mode"));
    }
    domain(4, 13); assert(send(applet) && result == 0x1c01);
    domain(4, 13); message[4] = 0x140001; message[1] = 11; message[12] = 0x010001;
    assert(send(applet) && !result);
    domain(4, 62); message[4] = 0x140001; message[1] = 11; message[12] = 2;
    assert(send(applet) && !result);
    domain(4, 63); assert(send(applet) && !result && message[12] == 2);
    domain(4, 1234); assert(!send(applet));
    domain(2, 20); assert(send(applet) && !result && message[12] == 6);
    domain(6, 23); assert(!send(applet)); // No inherited title-version response.
    domain(6, 21); assert(send(applet) && !result && !memcmp(message + 12, "en-US", 6));
    domain(6, 1); message[4] = 0x140001; message[1] = 11; message[12] = 2;
    assert(send(applet) && !result && message[12] == 7);
    domain(6, 1); message[4] = 0x140001; message[1] = 11; message[12] = 2;
    assert(send(applet) && !result && message[10] == 0x480);
    domain(7, 0); assert(send(applet) && !result && message[12] == 8);
    domain(8, 0); assert(send(applet) && !result && message[12] == 0x88);
    uint8_t *launch = host_reserve_guest(0x10000); assert(launch && !host_protect(launch, 0x10000, 3));
    storage_read_frame((uintptr_t)launch, 0x88, 0, 0);
    message[8] = 0x180001; message[9] = 8; message[14] = 11;
    assert(send(applet) && !result && *(uint32_t *)launch == 0xc79497ca && launch[4] == 1 && launch[8] == 1);
    storage_read_frame(1, 0x88, 0, 0); message[8] = 0x180001; message[9] = 8; message[14] = 11;
    assert(send(applet) && result == 0xe601);
    assert(!host_release(launch, 0x10000));
    for (unsigned i = 0; i < 2; i++) { r[0] = waits[i]; assert(kernel_svc(0x16, r) && !r[0]); }
    assert(service_close(applet));
}

static void check_account_settings(uint32_t sm) {
    get_service("acc:u0"); assert(send(sm) && !result); uint32_t account = message[3];
    cmif(5, 0); assert(send(account) && !result);
    domain(1, 150); assert(!send(account));
    memset(message, 0, sizeof message);
    message[0] = 6; message[1] = 0x8000000d; message[2] = 1;
    message[8] = 0x180001; message[9] = 1; message[12] = 0x49434653; message[14] = 140;
    assert(send(account) && !result);
    domain(1, 150); assert(send(account) && !result && !message[12]);
    domain(1, 5); message[4] = 0x200001; message[1] = 14;
    assert(send(account) && result == 0x1c01);
    domain(1, 5); message[4] = 0x200001; message[1] = 14; message[12] = 1;
    assert(send(account) && !result && message[12] == 2);
    cmif(5, 2); assert(send(account) && !result); uint32_t clone = message[3];
    assert(service_close(account));
    domain(2, 1); assert(send(clone) && !result && message[12] == 1 && !memcmp(message + 18, "Local Player", 13));
    assert(service_close(clone));
    get_service("set"); assert(send(sm) && !result); uint32_t settings = message[3];
    cmif(4, 0); assert(send(settings) && !result && !memcmp(message + 8, "en-US", 6));
    cmif(4, 2); message[8] = 15; assert(send(settings) && !result && !memcmp(message + 8, "zh-Hans", 8));
    cmif(4, 2); message[8] = UINT32_MAX; assert(send(settings) && result == 0x1c01);
    cmif(4, 6); assert(send(settings) && !result && message[8] == 18);
    cmif(4, 4); assert(send(settings) && !result && message[8] == 1);
    cmif(4, 999); assert(!send(settings));
    assert(service_close(settings));
}

static void hid_frame(unsigned command, unsigned value) {
    memset(message,0,sizeof message); message[0]=4; message[1]=0x8000000d; message[2]=1;
    message[8]=0x49434653; message[10]=command; message[12]=value;
}
extern void service_hid_style_changed(unsigned);
extern void service_hid_hold_changed(unsigned);
static void check_hid(uint32_t sm, uint32_t *wait) {
    get_service("hid"); assert(send(sm) && !result); uint32_t hid=message[3];
    cmif(4,106); assert(send(hid) && result); // PID and payload are required.
    hid_frame(106,8); assert(send(hid) && result);
    hid_frame(106,0); assert(send(hid) && !result && message[2]==2); uint32_t event=message[3]; *wait=event;
    uint64_t r[31]={0}; r[1]=(uintptr_t)wait; r[2]=1;
    assert(kernel_svc(0x18,r) && !r[0]);
    r[0]=event; assert(kernel_svc(0x12,r) && !r[0]);
    r[1]=(uintptr_t)wait; r[2]=1; r[3]=0; assert(kernel_svc(0x18,r) && r[0]==0xea01);
    service_hid_style_changed(0); r[1]=(uintptr_t)wait; r[2]=1; r[3]=0; assert(kernel_svc(0x18,r) && !r[0]);
    r[0]=event; assert(kernel_svc(0x12,r) && !r[0]);
    hid_frame(122,0); assert(send(hid) && !result);
    r[1]=(uintptr_t)wait; r[2]=1; r[3]=0; assert(kernel_svc(0x18,r) && !r[0]);
    hid_frame(120,1); assert(send(hid) && !result);
    hid_frame(121,0); assert(send(hid) && !result && message[8]==1);
    hid_frame(120,2); assert(send(hid) && result);
    service_hid_hold_changed(0); hid_frame(121,0); assert(send(hid) && !result && !message[8]);
    assert(service_close(hid));
    r[0]=event; assert(kernel_svc(0x12,r) && !r[0]);
    service_hid_style_changed(0); r[1]=(uintptr_t)wait; r[2]=1; r[3]=0; assert(kernel_svc(0x18,r) && !r[0]);
    r[0]=event; assert(kernel_svc(0x16,r) && !r[0]);
}
static void check_font(uint32_t sm) {
    get_service("pl:u"); assert(send(sm) && !result); uint32_t font=message[3];
    cmif(4,0); assert(send(font) && !result);
    cmif(4,1); assert(send(font) && !result && message[8]==1);
    cmif(4,2); assert(send(font) && !result && message[8]==56);
    cmif(4,3); assert(send(font) && !result && message[8]==8);
    cmif(4,2); message[8]=6; assert(send(font) && result);
    cmif(4,4); assert(send(font) && !result && message[2]==2); uint32_t handle=message[3];
    uint64_t r[31]={0}; r[1]=12; r[2]=0xffff8001; assert(kernel_svc(0x29,r) && !r[0]); uint64_t address=r[1]+0x10000;
    r[0]=handle; r[1]=address; r[2]=0x1100000; r[3]=3; assert(kernel_svc(0x13,r) && r[0]);
    r[0]=handle; r[3]=1; assert(kernel_svc(0x13,r) && !r[0]);
    assert(kernel_access(address,0x1100000,1) && !kernel_access(address,0x1100000,3));
    uint8_t header[12]; assert(host_read(header,address+8,sizeof header) && !memcmp(header,"\0\1\0\0",4));
    r[0]=handle; r[1]=address; r[2]=0x1100000; assert(kernel_svc(0x14,r) && !r[0]);
    r[0]=handle; assert(kernel_svc(0x16,r) && !r[0]); assert(service_close(font));
}
static void check_clock(uint32_t sm) {
    get_service("time:u"); assert(send(sm) && !result); uint32_t clock = message[3];
    cmif(5, 3); assert(send(clock) && !result && message[8] == 0x1000);
    cmif(5, 0); assert(send(clock) && !result);
    domain(1, 100); assert(send(clock) && !result && !message[12]);
    domain(1, 200); assert(send(clock) && !result && !message[12]);
    domain(1, 20); assert(send(clock) && !result && message[2] == 2); uint32_t shared = message[3];
    uint64_t regs[31] = {0}; regs[1] = 12; regs[2] = 0xffff8001;
    assert(kernel_svc(0x29, regs) && !regs[0]); uint64_t address = regs[1] + 0x10000;
    regs[0] = shared; regs[1] = address; regs[2] = 0x1000; regs[3] = 3;
    assert(kernel_svc(0x13, regs) && regs[0]); // Writable mapping must be rejected.
    regs[0] = shared; regs[3] = 1; assert(kernel_svc(0x13, regs) && !regs[0]);
    assert(kernel_access(address, 0x1000, 1) && !kernel_access(address, 0x1000, 3));
    uint32_t write = 1; assert(!host_write(address, &write, 4));
    const uint8_t *page = (const uint8_t *)(uintptr_t)address;
    TimeSystemContext context; memcpy(&context, page + 0x40, sizeof context);
    assert(!memcmp(page + 0x10, context.timestamp.source, 16));
    assert(!memcmp(page + 0x40, page + 0x60, 0x20) && !memcmp(page + 0x88, page + 0xa8, 0x20));
    assert(!memcmp(page + 0xd8, page + 0x110, 0x38) && !memcmp(page + 0x100, page + 0x10, 16));
    for (unsigned command = 0; command <= 4; command++) {
        domain(1, command); assert(send(clock) && !result); uint32_t child = message[12];
        if (command == 3) {
            domain(child, 101); message[4] = 0x180001; message[1] = 12;
            assert(!send(clock)); // Calendar/timezone rules remain explicit work.
            continue;
        }
        domain(child, 0); assert(send(clock) && !result);
        int64_t value; memcpy(&value, message + 12, 8);
        struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        if (command == 2) {
            assert(llabs(value - now.tv_sec) <= 1 && !memcmp(message + 14, context.timestamp.source, 16));
            domain(child, 200); assert(send(clock) && !result && !message[12] && !message[13]);
        } else {
            assert(llabs(value - (context.offset + now.tv_sec)) <= 1 && llabs(value - time(NULL)) <= 1);
            domain(child, 2); assert(send(clock) && !result && !memcmp(message + 12, &context, sizeof context));
            domain(child, 1); assert(!send(clock)); // Never alter host time or fabricate setter success.
        }
    }
    domain(1, 400); assert(!send(clock)); // Snapshot includes calendar data, outside the basic clock contract.
    regs[0] = shared; regs[1] = address; regs[2] = 0x1000;
    assert(kernel_svc(0x14, regs) && !regs[0]);
    regs[0] = shared; assert(kernel_svc(0x16, regs) && !regs[0]);
    uint8_t unread; assert(!host_read(&unread, address, 1));
    assert(service_close(clock));
}

#ifdef NSW_HEADLESS_AUDIO
static uint32_t *audio_out_frame(unsigned command,uint64_t input,size_t input_size,uint64_t output,size_t output_size,bool process) {
    memset(message,0,sizeof message); message[0]=4|(input_size?1u<<20:0)|(output_size?1u<<24:0);
    size_t cursor=process?24:8;
    if(process) {message[2]=3; message[5]=0xffff8001;}
    uint64_t addresses[]={input,output}; size_t sizes[]={input_size,output_size};
    for(unsigned i=0;i<2;i++) if(sizes[i]) {
        message[cursor/4]=(uint32_t)sizes[i]; message[cursor/4+1]=(uint32_t)addresses[i];
        message[cursor/4+2]=(uint32_t)(((addresses[i]>>32)&15)<<28)|((addresses[i]>>36)<<2); cursor+=12;
    }
    size_t raw=cursor,aligned=(cursor+15)&~(size_t)15;
    message[1]=(uint32_t)((aligned-raw+32)/4)|(process?0x80000000u:0);
    message[aligned/4]=0x49434653; message[aligned/4+2]=command;
    return message+aligned/4+4;
}
static void check_audio_out_ipc(uint32_t sm,void *stack) {
    get_service("audout:u"); assert(send(sm) && !result); uint32_t manager=message[3];
    uint8_t *memory=stack; memset(memory,0,0x8000);
    uint32_t *payload=audio_out_frame(3,(uintptr_t)memory,256,(uintptr_t)memory+256,256,true);
    payload[0]=48000; payload[1]=2;
    assert(send(manager) && !result && message[8]==48000 && message[9]==2 && message[10]==2 && message[11]==1);
    assert(!strcmp((char *)memory+256,"DeviceOut")); uint32_t output=message[3];
    cmif(4,4); assert(send(output) && !result && message[2]==2); uint32_t event=message[3];
    uint64_t *buffer=(void *)(memory+0x800); buffer[0]=0; buffer[1]=(uintptr_t)memory+0x1000; buffer[2]=4096; buffer[3]=960; buffer[4]=0;
    payload=audio_out_frame(7,(uintptr_t)buffer,40,0,0,false); payload[0]=111;
    assert(send(output) && !result);
    payload=audio_out_frame(7,(uintptr_t)buffer,40,0,0,false); payload[0]=111;
    assert(send(output) && result==0x1c01); // Tags remain owned until release retrieval.
    buffer[4]=4096; payload=audio_out_frame(7,(uintptr_t)buffer,40,0,0,false); payload[0]=222;
    assert(send(output) && result==0x1c01); buffer[4]=0; buffer[3]=1920;
    payload=audio_out_frame(7,(uintptr_t)buffer,40,0,0,false); payload[0]=222;
    assert(send(output) && !result);
    struct timespec delay={0,20000000}; nanosleep(&delay,NULL);
    cmif(4,10); assert(send(output) && !result && !message[8] && !message[9]);
    cmif(4,9); assert(send(output) && !result && message[8]==2);
    cmif(4,1); assert(send(output) && !result);
    uint64_t tags[2]={0}; unsigned total=0;
    for(unsigned tries=0;total<2 && tries<10;tries++) {
        memcpy(memory+0x400,&event,4); uint64_t r[31]={0,(uintptr_t)memory+0x400,1,1000000000};
        assert(kernel_svc(0x18,r) && !r[0]); r[0]=event; assert(kernel_svc(0x17,r) && !r[0]);
        audio_out_frame(8,0,0,(uintptr_t)memory+0x500,16,false); assert(send(output) && !result);
        unsigned count=message[8]; assert(total+count<=2); memcpy(tags+total,memory+0x500,count*8); total+=count;
    }
    assert(total==2 && tags[0]==111 && tags[1]==222);
    cmif(4,10); assert(send(output) && !result && message[8]==720 && !message[9]);
    cmif(4,2); assert(send(output) && !result);
    buffer[3]=1920; payload=audio_out_frame(3,(uintptr_t)buffer,40,0,0,false); payload[0]=333;
    assert(send(output) && !result);
    cmif(4,11); assert(send(output) && !result && message[8]==1);
    audio_out_frame(5,0,0,(uintptr_t)memory+0x500,8,false); assert(send(output) && !result && message[8]==1);
    assert(*(uint64_t *)(memory+0x500)==333);
    nanosleep(&delay,NULL); cmif(4,10); assert(send(output) && !result && message[8]==720);
    cmif(5,2); assert(send(output) && !result); uint32_t clone=message[3];
    assert(service_close(output)); cmif(4,0); assert(send(clone) && !result && message[8]==1);
    assert(service_close(clone) && service_close(manager));
    uint64_t r[31]={event}; assert(kernel_svc(0x16,r) && !r[0]);
}
static void check_audio_ipc(uint32_t sm,void *stack) {
    get_service("audren:u"); assert(send(sm) && !result); uint32_t manager=message[3];
    uint32_t config[13]={48000,240,2,0,1,0,0,0,0,0,0,0,0x37564552};
    cmif(4,1); message[1]=21; memcpy(message+8,config,sizeof config);
    assert(send(manager) && !result && message[8]==0x1000);
    uint64_t regs[31]={0,0x200000}; assert(kernel_svc(1,regs) && !regs[0]);
    uint64_t heap=regs[1]; regs[1]=heap; regs[2]=0x1000; regs[3]=0;
    assert(kernel_svc(0x15,regs) && !regs[0]); uint32_t transfer=(uint32_t)regs[1];
    memset(message,0,sizeof message); message[0]=4; message[1]=0x80000017; message[2]=5;
    message[5]=transfer; message[6]=0xffff8001; message[8]=0x49434653;
    memcpy(message+12,config,sizeof config); message[26]=0x1000;
    assert(send(manager) && !result); uint32_t renderer=message[3];
    cmif(4,0); assert(send(renderer) && !result && message[8]==48000);
    cmif(4,7); assert(send(renderer) && !result && message[2]==2); uint32_t event=message[3];
    cmif(4,5); assert(send(renderer) && !result);
    uint32_t header[16]={config[12],16,128,0x170,0x70,0,0,0,16}; header[15]=64;
    for(unsigned i=1;i<15;i++) header[15]+=header[i];
    uint8_t *input=(uint8_t *)stack+0x2000,*output=(uint8_t *)stack+0x4000;
    memset(input,0,header[15]); memcpy(input,header,64); memset(output,0xa5,4096);
    memset(message,0,sizeof message); message[0]=4|(1u<<20)|(2u<<24); message[1]=5;
    uint64_t addresses[3]={(uintptr_t)input,(uintptr_t)output,0}; uint32_t lengths[3]={header[15],4096,0};
    for(unsigned i=0;i<3;i++) {unsigned at=2+3*i; message[at]=lengths[i]; message[at+1]=(uint32_t)addresses[i]; message[at+2]=((addresses[i]>>32)&15)<<28 | ((addresses[i]>>36)<<2);}
    message[12]=0x49434653; message[14]=4;
    assert(send(renderer) && !result && !memcmp(output,&config[12],4));
    uint32_t written; memcpy(&written,output+60,4); assert(written<4096 && output[written]==0xa5);
    memcpy(stack,&event,4); regs[1]=(uintptr_t)stack; regs[2]=1; regs[3]=1000000000;
    assert(kernel_svc(0x18,regs) && !regs[0]);
    cmif(4,6); assert(send(renderer) && !result);
    assert(service_close(renderer) && service_close(manager));
    regs[0]=event; assert(kernel_svc(0x16,regs) && !regs[0]);
    regs[0]=transfer; assert(kernel_svc(0x16,regs) && !regs[0]);
}
#endif

int main(void) {
    LoadedImage image = {0}; uint32_t main_handle;
    void *stack = host_reserve_guest(0x10000); assert(stack && !host_protect(stack, 0x10000, 3));
    int initialized = kernel_init(&image, (uintptr_t)stack, 0x10000, &main_handle);
    if (initialized) return initialized;
    uint32_t sm;
    assert(!service_connect("sm:", &sm));
    assert(!service_set_access(NULL));
    assert(service_set_program(0x1234));
    assert(service_set_access("lm,fsp-srv,apm,appletOE,aoc:u,pctl,time:u,acc:u0,set,hid,pl:u,audren:u,audout:u,nvdrv,miss*,12345678"));
    for (const char **p = (const char *[]){",lm", "lm,", "ninechars", "a*b", "bad name", NULL}; *p; p++)
        assert(!service_set_access(*p));
    sm = connect_sm(); assert(!service_set_access("*"));
    get_service("lm"); assert(!send(sm)); // Registration is required.
    const uint32_t init[] = {16, 0x80000000, 1, 0, 0};
    for (size_t n = 0; n < sizeof init; n++) {
        void *short_message = malloc(n ? n : 1); assert(short_message);
        memcpy(short_message, init, n);
        service_request(sm, short_message, n, &result); assert(result);
        free(short_message);
    }
    assert(service_request(sm, NULL, 256, &result) && result == 0x1c01);
    register_sm(sm);
    // The second registration must stop, not reset shared state.
    memset(message, 0, sizeof message); message[0] = 16; message[1] = 0x80000000; message[2] = 1;
    assert(!send(sm) && !strcmp(service_blocker(), "repeated SM registration"));
    get_service("secret"); assert(send(sm) && !result && message[2] == 0x1015);
    get_service("nvdrv"); assert(!send(sm) && !strcmp(service_blocker(), "nvdrv"));
    get_service("missing"); assert(!send(sm) && !strcmp(service_blocker(), "missing"));
    get_service("12345678"); assert(!send(sm));
    get_service("lm"); message[3] = 0x01000000; assert(send(sm) && message[2] == 0x1015);
    get_service("lm"); assert(send(sm) && !result); uint32_t lm = message[3];
    cmif(7, 0); assert(send(lm) && !result && message[8] == 1);
    memset(message, 0, sizeof message);
    message[0] = 6; message[1] = 0x8000000d; message[2] = 1;
    message[8] = 0x180001; message[9] = 1; message[12] = 0x49434653; message[13] = 1;
    assert(send(lm) && !result && message[12] == 2);
    cmif(5, 2); assert(send(lm) && !result); uint32_t clone = message[3];
    assert(service_close(lm)); // The clone retains the domain and logger object.
    domain(17, 0); assert(send(clone) && result == 0x1c01);
    domain(1, 0); message[4] = 0xffff0001; assert(send(clone) && result == 0x1c01);
    void *packet = host_reserve_guest(0x20000); assert(packet && !host_protect(packet, 0x10000, 3));
    ((uint32_t *)packet)[5] = 4;
    logger_frame((uintptr_t)packet, 28); assert(send(clone) && !result);
    logger_frame(1, 28); assert(send(clone) && result == 0xe601);
    logger_frame((uintptr_t)packet + 0xfff0, 28); assert(send(clone) && result == 0xe601);
    logger_frame((uintptr_t)packet, 23); assert(send(clone) && result == 0x1c01);
    domain(2, 0); message[4] = 2; assert(send(clone) && !result);
    domain(2, 0); assert(send(clone) && result == 0x1c01);
    assert(service_close(clone)); assert(!host_release(packet, 0x20000));
    get_service("fsp-srv"); assert(send(sm) && !result); uint32_t fs = message[3];
    cmif(4, 1005); assert(!send(fs));
    cmif(5, 3); assert(send(fs) && !result && message[8] == 0x1000);
    cmif(5, 0); assert(send(fs) && !result);
    memset(message, 0, sizeof message);
    message[0] = 6; message[1] = 0x8000000d; message[2] = 1;
    message[8] = 0x180001; message[9] = 1; message[12] = 0x49434653; message[14] = 1;
    assert(send(fs) && !result);
    domain(1, 1005); assert(send(fs) && !result && !message[12]);
    domain(1, 200); assert(!send(fs) && !strcmp(service_blocker(), "merged RomFS unavailable (ZELDA_ROMFS)"));
    check_storage(fs);
    assert(service_close(fs));
    get_service("apm"); assert(send(sm) && !result); uint32_t apm = message[3];
    cmif(4, 1); assert(send(apm) && !result && message[8] == 1);
    cmif(4, 0); assert(send(apm) && !result); uint32_t performance = message[3];
    cmif(4, 1); message[8] = 1; assert(!send(performance));
    cmif(4, 0); message[8] = 1; message[9] = 0x12345678; assert(send(performance) && !result);
    cmif(4, 1); message[8] = 1; assert(send(performance) && !result && message[8] == 0x12345678);
    cmif(4, 0); message[8] = 2; assert(!send(performance));
    cmif(4, 1); message[8] = 1; assert(send(performance) && !result && message[8] == 0x12345678);
    assert(service_close(performance) && service_close(apm));
    check_applet(sm, stack);
    get_service("aoc:u"); assert(send(sm) && !result); uint32_t aoc = message[3];
    cmif(4, 50); assert(send(aoc) && !result);
    for (unsigned requested = 0; requested < 3; requested++) {
        uint64_t address = (uintptr_t)stack;
        *(uint64_t *)stack = UINT64_MAX;
        memset(message, 0, sizeof message); message[0] = 4 | (1u << 24); message[1] = 0x80000008; message[2] = 1;
        message[5] = 4; message[6] = (uint32_t)address;
        message[7] = (((address >> 32) & 15) << 28) | ((address >> 36) << 2);
        message[8] = 0x49434653; message[10] = 3; message[13] = requested;
        assert(send(aoc) && result == (requested <= 1 ? 0 : 0x1c01));
        if (!result) assert(!message[8]);
        assert(*(uint64_t *)stack == UINT64_MAX);
    }
    assert(service_close(aoc));
    get_service("pctl"); assert(send(sm) && !result); uint32_t pctl = message[3];
    cmif(4, 0); assert(!send(pctl)); // Missing PID and scalar input.
    memset(message, 0, sizeof message); message[0] = 4; message[1] = 0x8000000b; message[2] = 1; message[8] = 0x49434653;
    assert(send(pctl) && !result); uint32_t parent = message[3];
    cmif(4, 1); assert(send(parent) && !result);
    cmif(4, 99); assert(!send(parent));
    assert(service_close(parent) && service_close(pctl));
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&workers[i], NULL, independent_client, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
    uint32_t cmif_sm = connect_sm();
    memset(message, 0, sizeof message); message[0] = 4; message[1] = 0x8000000b; message[2] = 1;
    message[8] = 0x49434653; message[11] = 123;
    assert(send(cmif_sm) && !result && message[7] == 123);
    memset(message, 0, sizeof message); message[0] = 15;
    assert(send(cmif_sm) && !result); assert(send(cmif_sm) && result == 0xf601);
    assert(service_close(cmif_sm) && !service_close(cmif_sm));
    assert(send(cmif_sm) && result == 0xe401);
    uint32_t allocated[64]; unsigned count = 0;
    while (count < 64 && service_connect("sm:", &allocated[count])) count++;
    assert(count == 63);
    for (unsigned i = 0; i < count; i++) assert(service_close(allocated[i]));
    const uint32_t control[] = {5, 8, 0, 0, 0x49434653, 0, 3, 0, 0, 0};
    for (size_t n = 0; n <= sizeof control; n++) {
        uint8_t *bytes = malloc(n ? n : 1); assert(bytes);
        for (size_t i = 0; i < n; i++) {
            memcpy(bytes, control, n); bytes[i] ^= 0x80;
            service_request(sm, bytes, n, &result);
        }
        free(bytes);
    }
    check_clock(sm);
    check_account_settings(sm);
    check_hid(sm, stack);
    check_font(sm);
#ifdef NSW_HEADLESS_AUDIO
    check_audio_ipc(sm,stack);
    check_audio_out_ipc(sm,stack);
#else
    get_service("audren:u"); assert(!send(sm));
#endif
    service_shutdown(); assert(!service_close(sm));
    assert(service_set_access("")); sm = connect_sm(); register_sm(sm);
    get_service("lm"); assert(send(sm) && !result && message[2] == 0x1015);
    service_shutdown(); kernel_destroy(); assert(!host_release(stack, 0x10000)); puts("Basic IPC checks passed");
}
