// Store bounded NVN shader binaries at registration time. Pipeline translation
// and shader execution are separate capabilities, and still stop explicitly.
typedef struct {uint64_t serial; unsigned count; void *blocks[6]; size_t lengths[6];} ProgramRecord;
static ProgramRecord programs[256];
static pthread_mutex_t program_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t program_bytes;
static uint32_t program_word(const void *p,size_t offset) {uint32_t v; memcpy(&v,(const uint8_t *)p+offset,4); return v;}
static const prog_t *program_check(const NVNprogram *p) {const prog_t *o=READ(prog_t,p); if(o->magic!=MAGIC('V','K','P','G')) die("invalid NVN program"); return o;}
static void program_drop(ProgramRecord *record) {
    for(unsigned i=0;i<record->count;i++) {free(record->blocks[i]); program_bytes-=record->lengths[i];}
    *record=(ProgramRecord){0};
}
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *program,NVNdevice *device) {
    device_check(device); *WRITE(prog_t,program)=(prog_t){.magic=MAGIC('V','K','P','G')}; return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *program) {
    uint64_t serial=program_check(program)->serial; pthread_mutex_lock(&program_lock);
    for(unsigned i=0;i<256;i++) if(serial && programs[i].serial==serial) {program_drop(&programs[i]); break;}
    pthread_mutex_unlock(&program_lock); WRITE(prog_t,program)->magic=0;
}
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p,const char *label) {program_check(p); debug_label(label);}
static NVNboolean program_register(NVNprogram *program,int count,const NVNshaderData *shaders) {
    const prog_t *old=program_check(program); if(count<0 || count>6) return 0;
    size_t control_sizes[6],code_sizes[6],total=0; unsigned stages=0;
    for(int i=0;i<count;i++) {
        check(shaders[i].control,0x748,1); const void *ctl=shaders[i].control;
        control_sizes[i]=program_word(ctl,0x20); code_sizes[i]=program_word(ctl,0x704); unsigned stage=program_word(ctl,0x714);
        if(program_word(ctl,0)!=0x98761234 || control_sizes[i]<0x748 || control_sizes[i]>65536 || !code_sizes[i] || code_sizes[i]>(8u<<20) || stage>5 || (stages&(1u<<stage)))
            die("unsupported NVN shader control record");
        stages|=1u<<stage; check(ctl,control_sizes[i],1); check((void *)(uintptr_t)shaders[i].data,code_sizes[i],1);
        total+=control_sizes[i]+code_sizes[i];
    }
    pthread_mutex_lock(&program_lock); ProgramRecord *record=NULL;
    for(unsigned i=0;i<256;i++) if(old->serial && programs[i].serial==old->serial) {record=&programs[i]; break;}
    if(record) program_drop(record);
    else for(unsigned i=0;i<256;i++) if(!programs[i].serial) {record=&programs[i]; break;}
    if(!record || total>(128u<<20)-program_bytes) {pthread_mutex_unlock(&program_lock); die("NVN shader registration exceeds bounded storage");}
    prog_t *o=WRITE(prog_t,program); o->serial=atomic_fetch_add(&texture_serial,1)+1; o->nstage=count;
    record->serial=o->serial;
    for(int i=0;i<count;i++) {
        size_t length=control_sizes[i]+code_sizes[i]; void *block=malloc(length);
        if(!block) {program_drop(record); pthread_mutex_unlock(&program_lock); die("NVN shader snapshot allocation failed");}
        memcpy(block,shaders[i].control,control_sizes[i]); memcpy((uint8_t *)block+control_sizes[i],(void *)(uintptr_t)shaders[i].data,code_sizes[i]);
        record->blocks[i]=block; record->lengths[i]=length; record->count++; program_bytes+=length;
        o->sd[i]=(NVNshaderData){.control=block,.data=(uintptr_t)((uint8_t *)block+control_sizes[i])};
    }
    pthread_mutex_unlock(&program_lock); fprintf(stderr,"NVN registered %d deferred shader stages\n",count); return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *program,int count,const NVNshaderData *shaders) {
    if(count<0 || count>6) return 0; if(count) check(shaders,(size_t)count*sizeof *shaders,1);
    return program_register(program,count,shaders);
}
NVN(nvnProgramSetShadersExt, NVNboolean, NVNprogram *program,int count,const NvnShaderDataExt *shaders) {
    if(count<0 || count>6) return 0; if(count) check(shaders,(size_t)count*sizeof *shaders,1);
    NVNshaderData copy[6];
    for(int i=0;i<count;i++) {if(shaders[i].metadata) die("NVN shader extension metadata is unsupported"); copy[i]=shaders[i].shader;}
    return program_register(program,count,copy);
}
