// Offline shader packets contain the complete registered source for exact
// identity checking. The short file hash is an index, never a trust decision.
static uint64_t shader_index(const void *data,size_t size) {
    uint64_t hash=0xcbf29ce484222325ull;
    for(size_t i=0;i<size;i++) {hash^=((const uint8_t *)data)[i]; hash*=0x100000001b3ull;}
    return hash;
}
static void *shader_packet(const CommandState *state,const p_dispatch *dispatch,uint64_t serial,unsigned stage,size_t *length) {
    pthread_mutex_lock(&program_lock); const void *source=NULL; size_t source_size=0;
    for(unsigned i=0;i<256;i++) if(programs[i].serial==serial) {
        for(unsigned j=0;j<programs[i].count;j++) if(program_word(programs[i].blocks[j],0x714)==stage) {source=programs[i].blocks[j]; source_size=programs[i].lengths[j]; break;}
        break;
    }
    if(!source) {pthread_mutex_unlock(&program_lock); die("shader command has no registered stage %u",stage);}
    char path[128]; snprintf(path,sizeof path,"work/vulkan-shaders/cache/%016llx.vkcs",(unsigned long long)shader_index(source,source_size));
    FILE *file=fopen(path,"rb");
    if(!file) {pthread_mutex_unlock(&program_lock); shader_capture(state,OP_DISPATCH,dispatch,sizeof *dispatch); die("Vulkan shader packet missing; prepare the captured command");}
    uint32_t header[8];
    if(fread(header,1,sizeof header,file)!=sizeof header || header[0]!=0x4357534e || header[1]!=1 || header[2]!=source_size ||
        header[3]<20 || header[3]>(4u<<20) || header[3]%4 || header[4]<1 || header[4]>32 || (source_size&3)) {
        fclose(file); pthread_mutex_unlock(&program_lock); die("invalid Vulkan shader packet header");
    }
    size_t size=sizeof header+source_size+(size_t)header[4]*20+header[3];
    uint8_t *packet=malloc(size); if(!packet) {fclose(file); pthread_mutex_unlock(&program_lock); die("shader packet allocation failed");}
    memcpy(packet,header,sizeof header);
    bool ok=fread(packet+sizeof header,1,size-sizeof header,file)==size-sizeof header && fgetc(file)==EOF && !ferror(file) && !memcmp(source,packet+sizeof header,source_size);
    int closed=fclose(file); pthread_mutex_unlock(&program_lock);
    if(!ok || closed) {free(packet); die("Vulkan shader packet source mismatch or truncated data");}
    *length=size; return packet;
}
static void execute_compute(CommandState *state,const p_dispatch *dispatch) {
    if(!dispatch->x || !dispatch->y || !dispatch->z) return;
    size_t packet_size; uint8_t *packet=shader_packet(state,dispatch,state->programs[5],5,&packet_size); (void)packet_size;
    const uint32_t *header=(const uint32_t *)packet; unsigned count=header[4];
    const uint8_t *source=packet+32; size_t ctl_size=program_word(source,0x20);
    if(ctl_size<0x758 || ctl_size>header[2] || program_word(source,0x704)>header[2]-ctl_size) die("invalid compute source layout");
    for(unsigned i=0;i<3;i++) if(header[5+i]!=program_word(source,0x748+i*4)) die("Vulkan compute workgroup mismatch");
    uint32_t groups[3]={(uint32_t)dispatch->x,(uint32_t)dispatch->y,(uint32_t)dispatch->z};
    uint8_t *driver=calloc(1,65536); if(!driver) die("compute driver allocation failed");
    memcpy(driver+0x200,groups,12);
    for(unsigned i=0;i<16;i++) {
        NVNbufferRange uniform=i<14?state->uniform[5][i]:(NVNbufferRange){0};
        NVNbufferRange storage=state->storage[5][i];
        memcpy(driver+0x210+i*16,&uniform.address,8); uint32_t u_size=(uint32_t)uniform.size; memcpy(driver+0x218+i*16,&u_size,4);
        memcpy(driver+0x310+i*16,&storage.address,8); uint32_t s_size=(uint32_t)storage.size; memcpy(driver+0x318+i*16,&s_size,4);
    }
    VulkanComputeBuffer bindings[32]={0}; uint64_t addresses[32]={0}; size_t guest_sizes[32]={0}; bool writable[32]={0}; size_t total=0;
    const uint32_t *records=(const uint32_t *)(source+header[2]);
    for(unsigned i=0;i<count;i++) {
        const uint32_t *r=records+i*5; unsigned type=r[0],binding=r[1],slot=r[2],offset=r[3],flags=r[4];
        if(type>2 || flags>1) die("unsupported Vulkan shader binding record");
        NVNbufferRange range={0}; const void *input=NULL; size_t size=0;
        if(type==2) {
            if(binding || slot || offset || flags) die("invalid Vulkan support-buffer binding");
            size=2048;
        } else if(!type) {
            if(offset || flags) die("invalid Vulkan uniform binding");
            size=65536;
            if(slot==0) input=driver;
            else if(slot==1) {
                uint32_t c1=program_word(source,0x700),bytes=program_word(source,0x6fc),code_size=program_word(source,0x704);
                if(c1>code_size || bytes>code_size-c1 || bytes>65536) die("invalid Vulkan shader constants");
                range=(NVNbufferRange){(uintptr_t)(source+ctl_size+c1),bytes};
            } else if(slot>=3 && slot<17) range=state->uniform[5][slot-3];
            else die("unsupported compute constant-buffer slot");
            if(slot>1 && (!range.address || !range.size)) die("compute uniform buffer is unbound");
            if(range.size>65536) die("compute uniform size exceeds bound");
            if(slot>1) {check((void *)(uintptr_t)range.address,range.size,1); addresses[i]=range.address;}
        } else {
            if(slot || offset>65532/4) die("unsupported compute storage address bank");
            unsigned byte_offset=offset*4;
            if(byte_offset>=0x210 && byte_offset<0x210+14*16 && !(byte_offset&15)) range=state->uniform[5][(byte_offset-0x210)/16];
            else if(byte_offset>=0x310 && byte_offset<0x310+16*16 && !(byte_offset&15)) range=state->storage[5][(byte_offset-0x310)/16];
            else die("unsupported compute storage table offset 0x%x",byte_offset);
            if(!range.address || !range.size || range.size>(16u<<20)) die("compute storage buffer is unbound or too large");
            if(flags && byte_offset<0x310) die("compute shader writes a read-only uniform buffer");
            check((void *)(uintptr_t)range.address,range.size,flags?3:1);
            addresses[i]=range.address; size=range.size; writable[i]=flags!=0;
        }
        if(size>(64u<<20)-total) die("compute binding snapshots exceed bound"); total+=size;
        void *copy=calloc(1,size); if(!copy) die("compute binding snapshot allocation failed");
        if(input) memcpy(copy,input,size); else if(range.size) memcpy(copy,(void *)(uintptr_t)range.address,range.size);
        bindings[i]=(VulkanComputeBuffer){.binding=binding,.storage=type==1,.size=size,.input=copy,.output=writable[i]?copy:NULL};
        if(addresses[i]) guest_sizes[i]=range.size;
    }
    for(unsigned i=0;i<count;i++) if(writable[i]) for(unsigned j=0;j<count;j++) if(i!=j && addresses[j] &&
        addresses[i]<addresses[j]+guest_sizes[j] && addresses[j]<addresses[i]+guest_sizes[i]) die("aliased compute buffers require shared GPU storage");
    const uint32_t *spirv=records+count*5; char error[256];
    bool ok=vulkan_runtime_compute(spirv,header[3],groups,bindings,count,error); size_t changed=0,returned=0;
    if(ok) for(unsigned i=0;i<count;i++) if(writable[i]) {
        check((void *)(uintptr_t)addresses[i],bindings[i].size,3);
        const uint8_t *old=(const uint8_t *)(uintptr_t)addresses[i],*updated=bindings[i].output;
        for(size_t j=0;j<bindings[i].size;j++) changed+=old[j]!=updated[j];
        memcpy((void *)(uintptr_t)addresses[i],updated,bindings[i].size); returned+=bindings[i].size;
    }
    for(unsigned i=0;i<count;i++) free((void *)bindings[i].input); free(driver); free(packet);
    if(!ok) die("NVN Vulkan compute: %s",error);
    fprintf(stderr,"NVN Vulkan compute completed: groups=%u,%u,%u bindings=%u readback_bytes=%llu changed_bytes=%llu\n",
        groups[0],groups[1],groups[2],count,(unsigned long long)returned,(unsigned long long)changed);
}
