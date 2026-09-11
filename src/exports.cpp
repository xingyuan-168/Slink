#include "engine.h"
#include <cwchar>
#include <new>

namespace {
std::mutex apiMutex;
// Deliberately trivial global lifetime: never join a thread under loader lock.
// WM_Stop explicitly deletes the engine before a caller unloads the library.
gwm::Engine* engine=nullptr;
thread_local std::wstring errorText;
const wchar_t* describe(int code){switch(code){case GWM_OK:return L"成功";case GWM_INVALID_ARGUMENT:return L"参数无效";
case GWM_NOT_STARTED:return L"映射尚未启动或已关闭";case GWM_ALREADY_STARTED:return L"当前进程已有画布";
case GWM_CAPACITY_EXCEEDED:return L"网格容量不足或超过64格";case GWM_SLOT_OUT_OF_RANGE:return L"槽位编号超出范围";
case GWM_UNSUPPORTED_SYSTEM:return L"系统不支持窗口捕获";case GWM_GRAPHICS_FAILURE:return L"图形设备或窗口捕获失败";
case GWM_IMAGE_DECODE_FAILED:return L"图片解码失败，仅支持不超过4096像素边长的PNG/BMP";
case GWM_STOPPING:return L"映射正在停止";case GWM_BUFFER_TOO_SMALL:return L"输出缓冲区不足";
case GWM_ACTIVATION_DENIED:return L"系统拒绝激活窗口";default:return L"内部错误";}}
template<class F> int call(F action) noexcept {
    try{int code=action();errorText=code?describe(code):L"";return code;}
    catch(const std::bad_alloc&){try{errorText=L"内存不足";}catch(...){}return GWM_INTERNAL_ERROR;}
    catch(...){try{errorText=L"内部异常，操作未完成";}catch(...){}return GWM_INTERNAL_ERROR;}
}
template<class F> int withEngine(F action){std::lock_guard lock(apiMutex);if(!engine||engine->stopped())return GWM_NOT_STARTED;return action(*engine);}
template<class F> int slotAction(int slot,F action){return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& c) -> int {if(slot<1||slot>static_cast<int>(c.slots.size()))return GWM_SLOT_OUT_OF_RANGE;return action(c.slots[slot-1],e);});});}
void bind(gwm::Slot& s,uint32_t h,gwm::Engine& e){s.hwnd=h;s.generation=e.nextGeneration();s.state=h?GWM_WAITING:GWM_EMPTY;s.error=0;s.frameAt=0;s.frames=0;}
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)gwm::module=h;return TRUE;}

int32_t GWM_CALL WM_StartMapping(const uint32_t* hwnds,int32_t count,int32_t rows,int32_t cols,int32_t x,int32_t y,int32_t w,int32_t h,int32_t sx,int32_t sy,int32_t sw,int32_t sh){return call([&]() -> int {
    if(count<0||count>gwm::MaxSlots||(count&&!hwnds)||!gwm::validLayout(rows,cols)||w<=0||h<=0||w>16384||h>16384||!gwm::validRegion({sx,sy,sw,sh}))return GWM_INVALID_ARGUMENT;
    if(count>rows*cols)return GWM_CAPACITY_EXCEEDED;
    std::lock_guard lock(apiMutex);if(engine&&!engine->stopped())return GWM_ALREADY_STARTED;
    delete engine;engine=nullptr;
    gwm::Config config;config.rows=rows;config.cols=cols;config.defaults={sx,sy,sw,sh};
    config.slots.resize(count);for(int i=0;i<count;++i){config.slots[i].hwnd=hwnds[i];config.slots[i].roi=config.defaults;}
    auto created=std::make_unique<gwm::Engine>(std::move(config),x,y,w,h);int result=created->start();
    if(result)return result;engine=created.release();return GWM_OK;
});}
int32_t GWM_CALL WM_ReplaceWindows(const uint32_t* hwnds,int32_t count){return call([&]() -> int {
    if(count<0||count>gwm::MaxSlots||(count&&!hwnds))return GWM_INVALID_ARGUMENT;
    std::vector<uint32_t> values;if(count)values.assign(hwnds,hwnds+count);
    return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& c) -> int {
        if(count>c.rows*c.cols)return GWM_CAPACITY_EXCEEDED;
        auto replacement=c.slots;size_t old=replacement.size();replacement.resize(count);
        for(int i=0;i<count;++i){if(size_t(i)>=old)replacement[i].roi=c.defaults;bind(replacement[i],values[i],e);}
        c.slots.swap(replacement);return GWM_OK;
    });});
});}
int32_t GWM_CALL WM_SetWindow(int32_t slot,uint32_t hwnd){return call([&]() -> int {return slotAction(slot,[&](gwm::Slot& s,gwm::Engine& e) -> int {bind(s,hwnd,e);return GWM_OK;});});}
int32_t GWM_CALL WM_RemoveWindow(int32_t slot){return WM_SetWindow(slot,0);}
int32_t GWM_CALL WM_AddWindow(uint32_t hwnd,int32_t* out){return call([&]() -> int {if(!out)return GWM_INVALID_ARGUMENT;return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& c) -> int {if(c.slots.size()>=size_t(c.rows*c.cols))return GWM_CAPACITY_EXCEEDED;gwm::Slot s;s.roi=c.defaults;bind(s,hwnd,e);c.slots.push_back(std::move(s));*out=static_cast<int>(c.slots.size());return GWM_OK;});});});}
int32_t GWM_CALL WM_SetRegion(int32_t slot,int32_t x,int32_t y,int32_t w,int32_t h){return call([&]() -> int {if(!gwm::validRegion({x,y,w,h}))return GWM_INVALID_ARGUMENT;return slotAction(slot,[&](gwm::Slot& s,gwm::Engine&) -> int {s.roi={x,y,w,h};return GWM_OK;});});}
int32_t GWM_CALL WM_SetLayout(int32_t r,int32_t c){return call([&]() -> int {if(!gwm::validLayout(r,c))return GWM_INVALID_ARGUMENT;return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& cfg) -> int {if(size_t(r*c)<cfg.slots.size())return GWM_CAPACITY_EXCEEDED;cfg.rows=r;cfg.cols=c;return GWM_OK;});});});}
int32_t GWM_CALL WM_SetTitleW(int32_t slot,const wchar_t* text,int32_t size){return call([&]() -> int {if(size<0||size>4096||(size&&!text))return GWM_INVALID_ARGUMENT;std::wstring value;if(size)value.assign(text,text+size);return slotAction(slot,[&](gwm::Slot& s,gwm::Engine&) -> int {s.title=std::move(value);return GWM_OK;});});}
int32_t GWM_CALL WM_SetTitleA(int32_t slot,const char* text,int32_t size){return call([&]() -> int {
    if(size<0||size>16384||(size&&!text))return GWM_INVALID_ARGUMENT;
    std::wstring value;if(size){int count=MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,text,size,nullptr,0);if(count<=0||count>4096)return GWM_INVALID_ARGUMENT;value.resize(count);if(!MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,text,size,value.data(),count))return GWM_INVALID_ARGUMENT;}
    return slotAction(slot,[&](gwm::Slot& s,gwm::Engine&) -> int {s.title=std::move(value);return GWM_OK;});
});}
int32_t GWM_CALL WM_SetBackground(int32_t slot,const uint8_t* bytes,int32_t size){return call([&]() -> int {
    if(size<0||size>32*1024*1024||(size&&!bytes)||slot<0)return GWM_INVALID_ARGUMENT;
    std::shared_ptr<gwm::Image> image;
    if(size){try{image=gwm::decodeImage(bytes,size);}catch(...){return GWM_IMAGE_DECODE_FAILED;}}
    return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& c) -> int {if(slot>static_cast<int>(c.slots.size()))return GWM_SLOT_OUT_OF_RANGE;if(slot)c.slots[slot-1].background=std::move(image);else c.background=std::move(image);return GWM_OK;});});
});}
int32_t GWM_CALL WM_SetForceBackground(int32_t slot,int32_t enabled){return call([&]() -> int {if(enabled!=0&&enabled!=1)return GWM_INVALID_ARGUMENT;return slotAction(slot,[&](gwm::Slot& s,gwm::Engine&) -> int {s.force=enabled!=0;return GWM_OK;});});}
int32_t GWM_CALL WM_SetFrameRate(int32_t fps){return call([&]() -> int {if(fps!=15&&fps!=30&&fps!=60)return GWM_INVALID_ARGUMENT;return withEngine([&](gwm::Engine& e) -> int {return e.mutate([&](gwm::Config& c) -> int {c.fps=fps;return GWM_OK;});});});}
int32_t GWM_CALL WM_GetCanvasWindow(uint32_t* out){return call([&]() -> int {if(!out)return GWM_INVALID_ARGUMENT;return withEngine([&](gwm::Engine& e) -> int {*out=gwm::fromWindow(e.window());return GWM_OK;});});}
int32_t GWM_CALL WM_GetCanvasStatus(int32_t* state,int32_t* count,int32_t* rows,int32_t* cols){return call([&]() -> int {
    if(!state||!count||!rows||!cols)return GWM_INVALID_ARGUMENT;std::lock_guard lock(apiMutex);
    if(!engine){*state=GWM_STOPPED;*count=*rows=*cols=0;return GWM_OK;}auto view=engine->snapshot();*state=engine->canvasState();*count=static_cast<int>(view.slots.size());*rows=view.rows;*cols=view.cols;return GWM_OK;
});}
int32_t GWM_CALL WM_GetSlotStatus(int32_t slot,uint32_t* hwnd,int32_t* state,int32_t* error,uint32_t* age){return call([&]() -> int {
    if(!hwnd||!state||!error||!age)return GWM_INVALID_ARGUMENT;return withEngine([&](gwm::Engine& e) -> int {auto view=e.snapshot();if(slot<1||slot>static_cast<int>(view.slots.size()))return GWM_SLOT_OUT_OF_RANGE;const auto& s=view.slots[slot-1];*hwnd=s.hwnd;*state=s.state;*error=s.error;*age=s.frameAt?static_cast<uint32_t>(std::min<uint64_t>(gwm::nowMs()-s.frameAt,0xfffffffeULL)):0xffffffffU;return GWM_OK;});
});}
int32_t GWM_CALL WM_GetLastErrorTextW(wchar_t* buffer,int32_t capacity,int32_t* required){
    // A length probe must not overwrite the error that is being queried.
    if(!required||capacity<0||(capacity&&!buffer))return GWM_INVALID_ARGUMENT;
    *required=static_cast<int32_t>(errorText.size()+1);
    if(capacity<*required)return GWM_BUFFER_TOO_SMALL;
    std::copy(errorText.begin(),errorText.end(),buffer);buffer[errorText.size()]=0;return GWM_OK;
}
int32_t GWM_CALL WM_GetFrameCounters(int32_t slot,uint32_t* captured,uint32_t* presented,uint32_t* elapsed){return call([&]() -> int {
    if(slot<0||!captured||!presented||!elapsed)return GWM_INVALID_ARGUMENT;
    return withEngine([&](gwm::Engine& e) -> int {
        auto view=e.snapshot();if(slot>static_cast<int>(view.slots.size()))return GWM_SLOT_OUT_OF_RANGE;
        *captured=slot?static_cast<uint32_t>(view.slots[slot-1].frames):0;*presented=e.presented();*elapsed=e.elapsed();return GWM_OK;
    });
});}
int32_t GWM_CALL WM_Stop(){return call([]() -> int {std::lock_guard lock(apiMutex);if(engine){engine->stop();delete engine;engine=nullptr;}return GWM_OK;});}
#ifdef GWM_TEST_HOOKS
extern "C" __declspec(dllexport) int32_t GWM_CALL WM_TestInjectDeviceLoss(){return call([]() -> int {return withEngine([](gwm::Engine& e) -> int {e.injectDeviceLoss();return GWM_OK;});});}
#endif
