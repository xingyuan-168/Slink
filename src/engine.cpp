#include "engine.h"
#include "capture.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <map>
#include <set>

namespace gwm {
HINSTANCE module=nullptr;
static constexpr wchar_t ClassName[]=L"GameWindowMapper.Canvas.v1";
static constexpr UINT FinishClose=WM_APP+71;
Engine::Engine(Config c,int px,int py,int w,int h):config(std::move(c)),x(px),y(py),initialWidth(w),initialHeight(h) {
    for(auto& s:config.slots){s.generation=nextGeneration();s.state=s.hwnd?GWM_WAITING:GWM_EMPTY;}
}
Engine::~Engine(){stop();}
int Engine::start() {
    uiThread=std::thread([this]{uiMain();});
    std::unique_lock lock(wakeMutex);wake.wait(lock,[this]{return uiReady&&renderReady;});
    return startupError;
}
void Engine::requestStop() noexcept {stopRequested=true;state=GWM_CANVAS_STOPPING;wake.notify_all();}
void Engine::stop() {requestStop();if(uiThread.joinable())uiThread.join();state=GWM_STOPPED;}
int Engine::mutate(const std::function<int(Config&)>& action) {
    if(stopRequested)return GWM_NOT_STARTED;
    std::lock_guard lock(configMutex);if(stopRequested)return GWM_STOPPING;
    int result=action(config);wake.notify_all();return result;
}
Config Engine::snapshot(){std::lock_guard lock(configMutex);return config;}
void Engine::activate(int px,int py) {
    Hit target{};bool found=false;
    {
        std::lock_guard lock(configMutex);
        for(const auto& h:hits)if(PtInRect(&h.rect,{px,py})){
            if(h.slot<config.slots.size()&&config.slots[h.slot].generation==h.generation&&config.slots[h.slot].state!=GWM_INVALID){target=h;found=true;}
            break;
        }
    }
    if(!found||!target.hwnd)return;
    HWND h=toWindow(target.hwnd);if(!IsWindow(h))return;
    if(IsIconic(h))ShowWindowAsync(h,SW_RESTORE);
    bool activated=SetForegroundWindow(h)!=FALSE;
    if(!activated){FLASHWINFO info{sizeof(info),h,FLASHW_TRAY,2,0};FlashWindowEx(&info);}
    std::lock_guard lock(configMutex);
    if(target.slot<config.slots.size()&&config.slots[target.slot].generation==target.generation)
        config.slots[target.slot].error=activated?GWM_OK:GWM_ACTIVATION_DENIED;
}
LRESULT CALLBACK Engine::wndProc(HWND window,UINT message,WPARAM wp,LPARAM lp) noexcept {
    auto self=reinterpret_cast<Engine*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Engine*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(window,message,wp,lp);
    try{
        switch(message){
        case WM_CLOSE:self->requestStop();return 0;
        case FinishClose:DestroyWindow(window);return 0;
        case WM_DESTROY:self->requestStop();PostQuitMessage(0);return 0;
        case WM_LBUTTONDBLCLK:self->activate(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
        case WM_SIZE:self->wake.notify_all();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT p{};BeginPaint(window,&p);EndPaint(window,&p);return 0;}
        case WM_DPICHANGED:{const auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        case WM_GETMINMAXINFO:{auto p=reinterpret_cast<MINMAXINFO*>(lp);p->ptMinTrackSize={160,120};return 0;}
        }
    }catch(...){self->requestStop();}
    return DefWindowProcW(window,message,wp,lp);
}
void Engine::uiMain() noexcept {
    bool registered=false;
    auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try{
        WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_DBLCLKS|CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=wndProc;
        wc.hInstance=module;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.hIcon=LoadIconW(module,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;wc.lpszClassName=ClassName;
        if(!RegisterClassExW(&wc))throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));registered=true;
        RECT r{0,0,initialWidth,initialHeight};auto style=WS_OVERLAPPEDWINDOW;
        POINT point{x,y};auto monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
        (void)monitor; // Actual size is adjusted using the new window's own DPI below.
        AdjustWindowRectExForDpi(&r,style,FALSE,0,96);
        HWND h=CreateWindowExW(WS_EX_APPWINDOW,ClassName,L"GameWindowMapper · 屏幕墙",style,x,y,r.right-r.left,r.bottom-r.top,nullptr,nullptr,module,this);
        if(!h)throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));hwnd=h;
        UINT dpi=GetDpiForWindow(h);r={0,0,initialWidth,initialHeight};AdjustWindowRectExForDpi(&r,style,FALSE,WS_EX_APPWINDOW,dpi);
        SetWindowPos(h,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SendMessageW(h,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(wc.hIcon));SendMessageW(h,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(wc.hIconSm));
        BOOL dark=TRUE;DwmSetWindowAttribute(h,20,&dark,sizeof(dark));
        ShowWindow(h,SW_SHOWNORMAL);UpdateWindow(h);
        renderThread=std::thread([this]{renderMain();});
        {std::lock_guard lock(wakeMutex);uiReady=true;}wake.notify_all();
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }catch(...){
        requestStop();{std::lock_guard lock(wakeMutex);startupError=GWM_INTERNAL_ERROR;uiReady=true;if(!renderThread.joinable())renderReady=true;}wake.notify_all();
    }
    stopRequested=true;wake.notify_all();
    if(renderThread.joinable())renderThread.join();
    if(auto h=hwnd.exchange(nullptr);h&&IsWindow(h))DestroyWindow(h);
    if(registered)UnregisterClassW(ClassName,module);
    state=GWM_STOPPED;if(previous)SetThreadDpiAwarenessContext(previous);
}
void Engine::renderMain() noexcept {
    auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    std::unique_ptr<Graphics> graphics;
    std::map<uint64_t,std::unique_ptr<Capture>> captures;
    std::map<uint64_t,int> failures;
    try{
        winrt::check_hresult(co);
        if(!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()){
            std::lock_guard lock(wakeMutex);startupError=GWM_UNSUPPORTED_SYSTEM;
        }else graphics=std::make_unique<Graphics>(hwnd.load());
    }catch(...){std::lock_guard lock(wakeMutex);startupError=GWM_GRAPHICS_FAILURE;}
    {std::lock_guard lock(wakeMutex);renderReady=true;}wake.notify_all();
    if(!graphics){stopRequested=true;PostMessageW(hwnd.load(),FinishClose,0,0);if(SUCCEEDED(co))CoUninitialize();if(previous)SetThreadDpiAwarenessContext(previous);return;}
    state=GWM_RUNNING;
    int recoveryAttempts=0;
    while(!stopRequested){
        auto started=std::chrono::steady_clock::now();
        Config view;
        try{
            view=snapshot();
            if(!graphics){
                graphics=std::make_unique<Graphics>(hwnd.load());failures.clear();
            }
#ifdef GWM_TEST_HOOKS
            if(injectedFailures.exchange(0))throw winrt::hresult_error(DXGI_ERROR_DEVICE_REMOVED);
#endif
            winrt::check_hresult(graphics->device->GetDeviceRemovedReason());
            std::set<uint64_t> active;
            for(auto& slot:view.slots){
                active.insert(slot.generation);
                if(!slot.hwnd){slot.state=GWM_EMPTY;slot.frameAt=0;continue;}
                auto h=toWindow(slot.hwnd);
                if(h==hwnd.load()){slot.state=GWM_CAPTURE_ERROR;slot.error=GWM_INVALID_ARGUMENT;continue;}
                if(slot.state==GWM_INVALID){captures.erase(slot.generation);continue;}
                if(!IsWindow(h)||GetAncestor(h,GA_ROOT)!=h){slot.state=GWM_INVALID;captures.erase(slot.generation);continue;}
                auto found=captures.find(slot.generation);
                if(found==captures.end()&&!IsIconic(h)&&!failures.count(slot.generation)){
                    try{found=captures.emplace(slot.generation,std::make_unique<Capture>(h,*graphics)).first;}
                    catch(...){failures[slot.generation]=GWM_GRAPHICS_FAILURE;}
                }
                if(found!=captures.end()){
                    auto& cap=*found->second;
                    const auto framesBefore=cap.frames;
                    try{cap.update(*graphics);}
                    catch(const winrt::hresult_error& e){
                        if(e.code()==DXGI_ERROR_DEVICE_REMOVED||e.code()==DXGI_ERROR_DEVICE_RESET||e.code()==DXGI_ERROR_DEVICE_HUNG||e.code()==D2DERR_RECREATE_TARGET)throw;
                        cap.state=GWM_CAPTURE_ERROR;cap.error=GWM_GRAPHICS_FAILURE;
                    }catch(...){cap.state=GWM_CAPTURE_ERROR;cap.error=GWM_GRAPHICS_FAILURE;}
                    slot.state=cap.state;slot.frameAt=cap.frameAt;slot.frames+=cap.frames-framesBefore;
                    if(cap.error)slot.error=cap.error;
                }else if(IsIconic(h))slot.state=GWM_MINIMIZED;
                else {slot.state=GWM_CAPTURE_ERROR;slot.error=GWM_GRAPHICS_FAILURE;}
                if(slot.force&&slot.state!=GWM_INVALID)slot.state=GWM_FORCED_BACKGROUND;
            }
            for(auto it=captures.begin();it!=captures.end();)if(!active.count(it->first))it=captures.erase(it);else++it;
            for(auto it=failures.begin();it!=failures.end();)if(!active.count(it->first))it=failures.erase(it);else++it;
            std::set<Image*> activeImages;if(view.background)activeImages.insert(view.background.get());
            for(const auto& s:view.slots)if(s.background)activeImages.insert(s.background.get());
            for(auto it=graphics->images.begin();it!=graphics->images.end();)if(!activeImages.count(it->first))it=graphics->images.erase(it);else++it;
            RECT bounds{};HWND canvas=hwnd.load();GetClientRect(canvas,&bounds);
            std::vector<Hit> newHits;
            if(!IsIconic(canvas)&&bounds.right>0&&bounds.bottom>0){
                if(bounds.right!=graphics->width||bounds.bottom!=graphics->height)graphics->resize(bounds.right,bounds.bottom);
                graphics->setTextDpi(GetDpiForWindow(canvas));
                graphics->begin();float dpiScale=float(GetDpiForWindow(canvas))/96.0f;
                for(int i=0;i<view.rows*view.cols;++i){
                    RECT r=tileRect(bounds.right,bounds.bottom,view.rows,view.cols,i);
                    float header=std::min(float(r.bottom-r.top),30*dpiScale);
                    auto titleBox=D2D1::RectF(float(r.left),float(r.top),float(r.right),r.top+header);
                    auto content=D2D1::RectF(float(r.left),r.top+header,float(r.right),float(r.bottom));
                    graphics->fill(titleBox,0x20252d);
                    std::wstring title=std::to_wstring(i+1)+L"  ";
                    const Slot* s=i<static_cast<int>(view.slots.size())?&view.slots[i]:nullptr;
                    if(s&&!s->title.empty())title+=s->title;
                    else if(s&&s->hwnd){
                        wchar_t buffer[512]{};InternalGetWindowText(toWindow(s->hwnd),buffer,512);title+=buffer[0]?buffer:L"未命名窗口";
                    }else title+=L"空槽位";
                    auto textBox=titleBox;textBox.left+=8; textBox.right-=6;
                    graphics->text(title,textBox,0xe6eaf0);
                    graphics->dc->PushAxisAlignedClip(content,D2D1_ANTIALIAS_MODE_ALIASED);
                    if(s&&s->hwnd&&s->state==GWM_LIVE){
                        auto it=captures.find(s->generation);
                        if(it!=captures.end()){auto& cap=*it->second;graphics->drawRegion(cap.bitmap.get(),s->roi,cap.clientOffset,cap.clientSize,content);}
                    }else{
                        auto img=s&&s->background?s->background:view.background;
                        graphics->drawImage(graphics->image(img),content);
                        if(img){auto c=content;c.top=std::max(c.top,(content.top+content.bottom)*.5f-15);c.bottom=std::min(c.bottom,c.top+30);graphics->fill(c,0x20252d);}
                        graphics->text(stateText(s?s->state:GWM_EMPTY),content,0xadb5c3,true);
                    }
                    graphics->dc->PopAxisAlignedClip();
                    graphics->fill(D2D1::RectF(float(r.right-1),float(r.top),float(r.right),float(r.bottom)),0x353b46);
                    graphics->fill(D2D1::RectF(float(r.left),float(r.bottom-1),float(r.right),float(r.bottom)),0x353b46);
                    if(s)newHits.push_back({r,size_t(i),s->generation,s->hwnd});
                }
                if(graphics->end())++presentedFrames;
            }
            {
                std::lock_guard lock(configMutex);
                for(size_t i=0;i<view.slots.size()&&i<config.slots.size();++i)if(view.slots[i].generation==config.slots[i].generation){
                    config.slots[i].state=view.slots[i].state;config.slots[i].frameAt=view.slots[i].frameAt;config.slots[i].frames=view.slots[i].frames;
                    // Preserve a newly published activation result; capture failures take precedence.
                    if(view.slots[i].error==GWM_GRAPHICS_FAILURE)config.slots[i].error=view.slots[i].error;
                    if(view.slots[i].state==GWM_LIVE&&config.slots[i].error==GWM_GRAPHICS_FAILURE)config.slots[i].error=GWM_OK;
                }
                if(!newHits.empty())hits=std::move(newHits);
            }
            recoveryAttempts=0;state=GWM_RUNNING;
        }catch(...){
            captures.clear();failures.clear();graphics.reset();++recoveryAttempts;
            {std::lock_guard lock(configMutex);for(auto& s:config.slots)if(s.hwnd&&s.state!=GWM_INVALID){s.state=GWM_RECOVERING;s.error=GWM_GRAPHICS_FAILURE;s.frameAt=0;}}
            if(recoveryAttempts>=3){state=GWM_CANVAS_ERROR;break;}
            std::unique_lock lock(wakeMutex);wake.wait_for(lock,std::chrono::milliseconds(500),[this]{return stopRequested.load();});
        }
        auto delay=std::chrono::milliseconds(1000/std::max(1,view.fps));
        std::unique_lock lock(wakeMutex);wake.wait_until(lock,started+delay,[this]{return stopRequested.load();});
    }
    captures.clear();graphics.reset();
    // Keep an error window closable while queries expose ERROR rather than
    // endlessly rebuilding a lost driver. WM_Close wakes this wait.
    if(state==GWM_CANVAS_ERROR){std::unique_lock lock(wakeMutex);wake.wait(lock,[this]{return stopRequested.load();});}
    PostMessageW(hwnd.load(),FinishClose,0,0);
    if(SUCCEEDED(co))CoUninitialize();if(previous)SetThreadDpiAwarenessContext(previous);
}
}
