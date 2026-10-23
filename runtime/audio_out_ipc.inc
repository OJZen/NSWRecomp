// Original NSWRecomp code (MIT). AudioOut CMIF buffer/ownership contract.
// Included inside request() only for the explicit silent audio backend.
    if (!control && (kind==AUD_OUT_MANAGER || kind==AUD_OUT)) {
        if (statics>1 || sends>1 || receives>1 || exchanges) return true;
        uint64_t input_address=0,input_length=0,output_address=receive_address,output_length=receive_length;
        if (statics) {
            uint32_t d=word(bytes,descriptors); input_length=d>>16;
            input_address=word(bytes,descriptors+4)|((uint64_t)((d>>12)&15)<<32)|((uint64_t)((d>>6)&63)<<36);
        }
        if (sends) {
            uint64_t address,length; mapped_buffer(bytes,descriptors+statics*8,&address,&length);
            if (length) { if(input_length) return true; input_address=address; input_length=length; }
        }
        if (receives) {
            uint64_t address,length; mapped_buffer(bytes,descriptors+statics*8+sends*12,&address,&length);
            if (length) { if(output_length) return true; output_address=address; output_length=length; }
        }
        if (kind==AUD_OUT_MANAGER && (command==0 || command==2)) {
            if (input_length || output_length<256) return true;
            const char name[256]="DeviceOut"; uint32_t count=1;
            if (!host_write(output_address,name,sizeof name)) { *result=0xe601; return true; }
            if (reply(bytes,size,tipc,token,0,domain,0,&count,sizeof count)) *result=0;
            return true;
        }
        if (kind==AUD_OUT_MANAGER && (command==1 || command==3)) {
            if (!pid || !copied || data_size<16 || input_length<1 || input_length>256 || output_length<256) return true;
            char name[256]={0};
            if (!host_read(name,input_address,(size_t)input_length)) { *result=0xe601; return true; }
            if (!memchr(name,0,(size_t)input_length) || (*name && strcmp(name,"DeviceOut"))) return blocked("unsupported AudioOut device");
            unsigned rate=word(bytes,payload),channels=word(bytes,payload+4)&0xffff;
            if (!rate) rate=48000; if (!channels) channels=2;
            uint32_t out[4]={rate,channels,2,1}; // PCM16, initially stopped.
            const char selected[256]="DeviceOut";
            if (!host_write(output_address,selected,sizeof selected)) { *result=0xe601; return true; }
            if (!audio_out_open(rate,channels)) return blocked("unsupported or duplicate silent AudioOut");
            if (domain) {
                for (unsigned i=0;i<16;i++) if(!s->objects[i]) {
                    if (reply(bytes,size,tipc,token,0,true,i+1,out,sizeof out)) { s->objects[i]=AUD_OUT; *result=0; }
                    else audio_out_close();
                    return true;
                }
                audio_out_close(); *result=0xce01; return true;
            }
            uint32_t child=open_session(AUD_OUT,NULL);
            if (!child) { audio_out_close(); *result=0xce01; return true; }
            if (reply(bytes,size,tipc,token,child,false,0,out,sizeof out)) *result=0;
            else close_session(child);
            return true;
        }
        if (kind==AUD_OUT && (command==3 || command==7)) {
            if (data_size<8 || input_length!=40 || output_length) return true;
            uint64_t buffer[5];
            if (!host_read(buffer,input_address,sizeof buffer)) { *result=0xe601; return true; }
            if (!audio_out_append(wide(bytes,payload),buffer)) return true;
            if (reply(bytes,size,tipc,token,0,domain,0,NULL,0)) *result=0;
            return true;
        }
        if (kind==AUD_OUT && (command==5 || command==8)) {
            if (input_length || !output_length || output_length%8 || output_length>64*8) return true;
            if (!kernel_access(output_address,(size_t)output_length,2)) { *result=0xe601; return true; }
            uint64_t tags[64]; uint32_t count=(uint32_t)audio_out_released(tags,(size_t)output_length/8);
            if (!host_write(output_address,tags,count*8)) { *result=0xe601; return true; }
            if (reply(bytes,size,tipc,token,0,domain,0,&count,sizeof count)) *result=0;
            return true;
        }
        if (kind==AUD_OUT && !input_length && !output_length && command<=13) {
            uint64_t value=0; size_t length=0;
            switch(command) {
            case 0: case 9: case 10: value=audio_out_value(command); length=command==10?8:4; break;
            case 1: case 2: if(!audio_out_start(command==1)) return true; break;
            case 4: {
                uint32_t handle=audio_out_event(); if(!handle) { *result=0xce01; return true; }
                if(reply(bytes,size,tipc,token,handle,domain,0,NULL,0)) { uint32_t descriptor=2; memcpy(bytes+8,&descriptor,4); *result=0; }
                else { uint64_t r[31]={handle}; kernel_svc(0x16,r); }
                return true;
            }
            case 6: if(data_size<8) return true; value=audio_out_contains(wide(bytes,payload)); length=1; break;
            case 11: value=audio_out_flush(); length=1; break;
            case 12: case 13: {
                float volume=0;
                if(command==12) { if(data_size<4) return true; memcpy(&volume,bytes+payload,4); }
                if(!audio_out_volume(&volume,command==12)) return true;
                memcpy(&value,&volume,4); length=command==13?4:0; break;
            }
            default: return blocked("unsupported silent AudioOut command");
            }
            if(reply(bytes,size,tipc,token,0,domain,0,&value,length)) *result=0;
            return true;
        }
        return blocked("unsupported silent AudioOut buffers or command");
    }
