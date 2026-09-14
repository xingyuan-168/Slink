#include "engine.h"
#include <cstring>
#include <new>

namespace {
std::mutex apiMutex;
bool mappingActive=false;

class Reader {
public:
    Reader(const uint8_t* p,size_t n):data(p),remaining(n){}
    const uint8_t* take(size_t n){if(n>remaining)throw GWM_INVALID_ARGUMENT;auto p=data;data+=n;remaining-=n;return p;}
    int32_t integer(){int32_t value;std::memcpy(&value,take(4),4);return value;}
    bool empty() const {return remaining==0;}
private:
    const uint8_t* data;size_t remaining;
};
std::shared_ptr<gwm::Image> image(Reader& r,int size){
    if(size<0||size>32*1024*1024)throw GWM_INVALID_ARGUMENT;
    if(!size)return {};
    auto bytes=r.take(size);
    try{return gwm::decodeImage(bytes,size);}catch(...){throw GWM_IMAGE_DECODE_FAILED;}
}
void options(gwm::Config& c,const uint8_t* data,int size){
    if(!size)return;
    Reader r(data,size);
    if(r.integer()!=1)throw GWM_INVALID_ARGUMENT;
    int fps=r.integer(),backgroundBytes=r.integer(),records=r.integer();
    if(fps!=0&&fps!=15&&fps!=30&&fps!=60)throw GWM_INVALID_ARGUMENT;
    if(records<0||records>static_cast<int>(c.slots.size()))throw GWM_INVALID_ARGUMENT;
    c.fps=fps?fps:30;c.background=image(r,backgroundBytes);
    bool seen[gwm::MaxSlots]{};
    for(int i=0;i<records;++i){
        int slot=r.integer();gwm::Region roi{r.integer(),r.integer(),r.integer(),r.integer()};
        int titleBytes=r.integer(),imageBytes=r.integer(),flags=r.integer();
        if(slot<1||slot>static_cast<int>(c.slots.size()))throw GWM_SLOT_OUT_OF_RANGE;
        if(seen[slot-1]||flags<0||(flags&~7)||titleBytes<0||titleBytes>16384)throw GWM_INVALID_ARGUMENT;
        seen[slot-1]=true;auto& s=c.slots[slot-1];
        if(flags&1){if(!gwm::validRegion(roi))throw GWM_INVALID_ARGUMENT;s.roi=roi;}
        s.force=(flags&2)!=0;
        auto title=r.take(titleBytes);
        if(flags&4){
            if(titleBytes%2||titleBytes>8192)throw GWM_INVALID_ARGUMENT;
            s.title.resize(titleBytes/2);if(titleBytes)std::memcpy(s.title.data(),title,titleBytes);
        }else if(titleBytes){
            int n=MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(title),titleBytes,nullptr,0);
            if(n<=0||n>4096)throw GWM_INVALID_ARGUMENT;
            s.title.resize(n);
            if(!MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(title),titleBytes,s.title.data(),n))throw GWM_INVALID_ARGUMENT;
        }
        s.background=image(r,imageBytes);
    }
    if(!r.empty())throw GWM_INVALID_ARGUMENT;
}
struct Startup {
    std::mutex mutex;std::condition_variable ready;bool done=false;int result=GWM_INTERNAL_ERROR;
    void publish(int code){std::lock_guard lock(mutex);result=code;done=true;ready.notify_all();}
};
struct Job {
    std::unique_ptr<gwm::Engine> engine;
    std::shared_ptr<Startup> startup;
    HMODULE module;
};
DWORD WINAPI runMapping(void* argument) noexcept {
    HMODULE reference;
    {
        std::unique_ptr<Job> job(static_cast<Job*>(argument));reference=job->module;
        try{
            int result=job->engine->start();
            job->startup->publish(result);
            job->engine->wait(); // UI joins rendering; owner joins UI.
        }catch(...){job->startup->publish(GWM_INTERNAL_ERROR);}
        // Destroy the engine, copied inputs and joined thread handles BEFORE
        // permitting another canvas. No Stop or subsequent Start is required.
    }
    {std::lock_guard lock(apiMutex);mappingActive=false;}
    // A temporary reference protects every DLL thread if the host calls
    // FreeLibrary early. Release it atomically with exit, never permanently pin.
    FreeLibraryAndExitThread(reference,0);
}
}

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)gwm::module=h;return TRUE;}

int32_t GWM_CALL WM_StartMapping(const uint32_t* hwnds,int32_t count,int32_t rows,int32_t cols,
    int32_t x,int32_t y,int32_t w,int32_t h,int32_t sx,int32_t sy,int32_t sw,int32_t sh,
    const uint8_t* configuration,int32_t configurationBytes){
    try{
        std::unique_lock lock(apiMutex);
        if(mappingActive)return GWM_ALREADY_STARTED;
        if(count<0||count>gwm::MaxSlots||(count&&!hwnds)||!gwm::validLayout(rows,cols)||
            w<=0||h<=0||w>16384||h>16384||!gwm::validRegion({sx,sy,sw,sh})||
            configurationBytes<0||configurationBytes>256*1024*1024||(configurationBytes&&!configuration))return GWM_INVALID_ARGUMENT;
        if(count>rows*cols)return GWM_CAPACITY_EXCEEDED;
        gwm::Config config;config.rows=rows;config.cols=cols;config.defaults={sx,sy,sw,sh};
        config.slots.resize(count);
        for(int i=0;i<count;++i){config.slots[i].hwnd=hwnds[i];config.slots[i].roi=config.defaults;}
        options(config,configuration,configurationBytes);
        auto startup=std::make_shared<Startup>();
        auto job=std::make_unique<Job>();job->startup=startup;
        job->engine=std::make_unique<gwm::Engine>(std::move(config),x,y,w,h);
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&gwm::module),&job->module))return GWM_INTERNAL_ERROR;
        mappingActive=true;
        // Transfer ownership before the worker can run, including fast failures.
        auto argument=job.release();
        HANDLE thread=CreateThread(nullptr,0,runMapping,argument,0,nullptr);
        if(!thread){job.reset(argument);mappingActive=false;FreeLibrary(job->module);return GWM_INTERNAL_ERROR;}
        CloseHandle(thread);
        lock.unlock();
        std::unique_lock ready(startup->mutex);
        startup->ready.wait(ready,[&]{return startup->done;});
        return startup->result;
    }catch(GWM_Result result){return result;}
    catch(...){return GWM_INTERNAL_ERROR;}
}
