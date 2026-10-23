// Explicit, bounded development capture. Derived data stays under ignored work/.
// This records a command that cannot execute; it never counts as GPU work.
static void shader_capture_file(const char *name,const void *data,size_t size) {
    char path[160]; snprintf(path,sizeof path,"work/vulkan-shaders/%s",name);
    FILE *file=fopen(path,"wb"); if(!file) die("cannot open Vulkan shader capture output");
    bool complete=fwrite(data,1,size,file)==size; int closed=fclose(file);
    if(!complete || closed) die("cannot write Vulkan shader capture output");
}
static void shader_capture(const CommandState *state,unsigned operation,const void *command,size_t command_bytes) {
    const char *enabled=getenv("NSW_VULKAN_CAPTURE"); if(!enabled || strcmp(enabled,"1")) return;
    static bool captured; if(captured) return; captured=true;
    FILE *file=fopen("work/vulkan-shaders/command.json","w");
    if(!file) die("create work/vulkan-shaders before enabling shader capture");
    fprintf(file,"{\"version\":1,\"origin\":%u,\"depth_mode\":%u,\"operation\":%u,\"command\":[",state->origin,state->depth_mode,operation);
    for(size_t i=0;i<command_bytes/4;i++) {uint32_t word; memcpy(&word,(const uint8_t *)command+i*4,4); fprintf(file,"%s%u",i?",":"",word);}
    fputs("],\"viewport\":[",file);
    for(unsigned i=0;i<6;i++) fprintf(file,"%s%.9g",i?",":"",state->viewports[0][i]);
    fprintf(file,"],\"scissor\":[%d,%d,%d,%d],\"targets\":[",state->scissor.x,state->scissor.y,state->scissor.w,state->scissor.h);
    for(int i=0;i<state->rt.count;i++) fprintf(file,"%s{\"serial\":%llu,\"format\":%u,\"width\":%u,\"height\":%u}",i?",":"",(unsigned long long)state->rt.colors[i].serial,state->rt.colors[i].d.format,state->rt.colors[i].d.width,state->rt.colors[i].d.height);
    fputs("],\"draw_states\":[",file);
    for(unsigned i=0;i<19;i++) {fprintf(file,"%s\"",i?",":""); for(unsigned j=0;j<24;j++) fprintf(file,"%02x",state->draw_states[i][j]); fputc('"',file);}
    fputs("],\"vertex\":[",file);
    for(unsigned i=0;i<16;i++) {
        va_st attribute; memcpy(&attribute,&state->attributes[i],sizeof attribute);
        if(state->vertex[i].size) {char name[64]; snprintf(name,sizeof name,"vertex-%u.bin",i); size_t size=state->vertex[i].size<256?state->vertex[i].size:256;
            check((void *)(uintptr_t)state->vertex[i].address,size,1); shader_capture_file(name,(void *)(uintptr_t)state->vertex[i].address,size);}
        fprintf(file,"%s{\"format\":%u,\"offset\":%u,\"stream\":%u,\"stride\":%u,\"divisor\":%u,\"address\":%llu,\"size\":%llu}",
            i?",":"",attribute.fmt,attribute.offset,attribute.stream,state->streams[i].stride,state->streams[i].divisor,(unsigned long long)state->vertex[i].address,(unsigned long long)state->vertex[i].size);
    }
    fputs("],\"stages\":[",file); unsigned stages=0; size_t captured_bytes=0;
    pthread_mutex_lock(&program_lock);
    for(unsigned stage=0;stage<6;stage++) {
        if(!state->programs[stage] || (operation==OP_DISPATCH ? stage!=5 : stage==5)) continue;
        ProgramRecord *record=NULL;
        for(unsigned i=0;i<256;i++) if(programs[i].serial==state->programs[stage]) {record=&programs[i]; break;}
        if(!record) die("capture references a finalized Vulkan program");
        for(unsigned i=0;i<record->count;i++) if(program_word(record->blocks[i],0x714)==stage) {
            char name[64]; snprintf(name,sizeof name,"stage-%u.nvn",stage);
            shader_capture_file(name,record->blocks[i],record->lengths[i]);
            fprintf(file,"%s{\"stage\":%u,\"source\":\"%s\",\"uniform\":[",stages++?",":"",stage,name);
            for(unsigned j=0;j<14;j++) {
                NVNbufferRange range=state->uniform[stage][j];
                if(range.size>65536 || range.size>(4u<<20)-captured_bytes) die("Vulkan uniform capture limit");
                snprintf(name,sizeof name,"stage-%u-ubo-%u.bin",stage,j);
                if(range.size) {check((void *)(uintptr_t)range.address,range.size,1); shader_capture_file(name,(void *)(uintptr_t)range.address,range.size); captured_bytes+=range.size;}
                fprintf(file,"%s{\"address\":%llu,\"size\":%llu,\"file\":\"%s\"}",j?",":"",(unsigned long long)range.address,(unsigned long long)range.size,name);
            }
            fputs("],\"storage\":[",file);
            for(unsigned j=0;j<16;j++) {
                NVNbufferRange range=state->storage[stage][j];
                fprintf(file,"%s{\"address\":%llu,\"size\":%llu}",j?",":"",(unsigned long long)range.address,(unsigned long long)range.size);
            }
            fputs("],\"textures\":[",file);
            for(unsigned j=0;j<32;j++) fprintf(file,"%s%llu",j?",":"",(unsigned long long)state->textures[stage][j]);
            fputs("],\"images\":[",file);
            for(unsigned j=0;j<32;j++) fprintf(file,"%s%llu",j?",":"",(unsigned long long)state->images[stage][j]);
            fputs("]}",file);
        }
    }
    pthread_mutex_unlock(&program_lock);
    fputs("],\"descriptors\":[",file); unsigned count=0;
    pthread_mutex_lock(&descriptor_lock);
    for(unsigned i=0;i<4096;i++) {
        const Descriptor *d=&descriptors[i]; if(!d->address || d->kind==3) continue;
        uint64_t start=state->texture_pool.pool?(uintptr_t)pool_range((const NVNmemoryPool *)state->texture_pool.pool,state->texture_pool.off,(size_t)state->texture_pool.n*32,1):0;
        if(d->address<start || d->address-start>=(uint64_t)state->texture_pool.n*32) continue;
        fprintf(file,"%s{\"id\":%llu,\"kind\":%u,\"serial\":%llu,\"target\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"levels\":%u}",
            count++?",":"",(unsigned long long)((d->address-start)/32),d->kind,(unsigned long long)d->texture.serial,
            d->texture.d.target,d->texture.d.format,d->texture.d.width,d->texture.d.height,d->texture.d.depth,d->texture.d.levels);
    }
    pthread_mutex_unlock(&descriptor_lock);
    fputs("]}\n",file); bool complete=!ferror(file); int closed=fclose(file);
    if(!complete || closed) die("cannot write Vulkan command capture");
    fprintf(stderr,"NVN captured pending shader command under work/vulkan-shaders\n");
}
