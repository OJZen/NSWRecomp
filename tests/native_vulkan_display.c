// Synthetic VI lifecycle, preview and callback/event contracts; no GPU.
#include <assert.h>
#include "vulkan_display.c"
static uint64_t output_memory[8];
static atomic_uint signals;
static unsigned handles,callbacks;
void die(const char *format,...) {(void)format; abort();}
void startup_window_closed(void) {abort();} // Finalize must suppress the user-close path.
bool vulkan_runtime_ready(void) {return true;}
bool kernel_access(uint64_t address,size_t size,unsigned permissions) {
    return permissions==3 && address==(uintptr_t)output_memory && size<=sizeof output_memory;
}
bool kernel_event_create(uint32_t *write,uint32_t *read) {assert(!handles); *write=1; *read=2; handles=2; return true;}
uint32_t kernel_event_copy(uint32_t handle) {assert(handle==2 && handles==2); return 3;}
bool kernel_svc(uint32_t number,uint64_t *r) {
    if(number==0x11) {assert(r[0]==1); atomic_fetch_add(&signals,1);}
    else {assert(number==0x16 && handles && (r[0]==1 || r[0]==2)); handles--;}
    r[0]=0; return true;
}
bool guest_callback_address(uint64_t address) {return address==0x7100001000;}
bool guest_callback(Cpu *caller,uint64_t address,const uint64_t *args,size_t count,uint64_t *result) {
    assert(caller && address==0x7100001000 && count==4 && !result);
    assert(args[0]==(uintptr_t)output_memory && args[1]==3 && args[2]==1 && args[3]==1); callbacks++; return true;
}
int main(void) {
    assert(!_putenv_s("NSW_VSYNC_ATTACH","7100001000")); Cpu c={0};
    for(unsigned pass=0;pass<2;pass++) {
        assert(!_putenv_s("NSW_VULKAN_WINDOW",pass?"1":""));
        assert(vulkan_display_window_requested()==(pass!=0));
        hle__ZN2nn2vi10InitializeEv(&c); assert(handles==2);
        char error[256]; bool queued=true; uint8_t pixels[4*2*4]={0};
        assert(vulkan_display_frame(pixels,sizeof pixels,4,2,&queued,error) && queued==(pass!=0));
        if(pass) {
            ULONGLONG end=GetTickCount64()+3000;
            while(!vulkan_display_window_stats().painted && GetTickCount64()<end) Sleep(10);
            HostWindowStats stats=vulkan_display_window_stats();
            assert(stats.created && !stats.closed && stats.submitted==1 && stats.painted==1);
            // A fatal report remains readable if its caller already owns the presenter lock.
            assert(!pthread_mutex_lock(&preview_lock));
            assert(vulkan_display_window_stats().submitted==1);
            assert(!pthread_mutex_unlock(&preview_lock));
        } else assert(!vulkan_display_window_stats().created);
        c.x[0]=(uintptr_t)output_memory; hle__ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE(&c); assert(!c.x[0]);
        uint64_t d=output_memory[0]; c.x[0]=(uintptr_t)output_memory; c.x[1]=d;
        hle__ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE(&c); assert(!c.x[0] && callbacks==pass+1);
        c.x[0]=(uintptr_t)output_memory; c.x[1]=d; hle__ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE(&c); assert(!c.x[0]);
        uint64_t l=output_memory[0]; c.x[0]=l; c.x[1]=2; c.x[2]=3; c.x[3]=640; c.x[4]=480;
        hle__ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii(&c); assert(!c.x[0] && crop[0]==2 && crop[3]==480);
        c.x[0]=(uintptr_t)output_memory; c.x[1]=l; hle__ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(&c);
        assert(!c.x[0] && vulkan_display_native((void *)output_memory[0]));
        c.x[0]=d; hle__ZN2nn2vi12CloseDisplayEPNS0_7DisplayE(&c); assert(c.x[0]==0x202);
        c.x[0]=l; hle__ZN2nn2vi12DestroyLayerEPNS0_5LayerE(&c); assert(!c.x[0]);
        c.x[0]=(uintptr_t)output_memory; c.x[1]=l; hle__ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(&c); assert(c.x[0]==0x202);
        Sleep(70); assert(atomic_load(&signals)>0);
        c.x[0]=d; hle__ZN2nn2vi12CloseDisplayEPNS0_7DisplayE(&c); assert(!c.x[0]);
        hle__ZN2nn2vi8FinalizeEv(&c); assert(!handles && !initialized && !running);
        assert(!preview);
        assert(vulkan_display_frame(pixels,sizeof pixels,4,2,&queued,error) && !queued);
        assert(vulkan_display_window_stats().painted==pass);
    }
    return 0;
}
