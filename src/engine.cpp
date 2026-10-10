#include "engine.h"
#include "capture.h"
#include "virtual/ui_ids.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <map>
#include <set>

namespace gwm {
HINSTANCE module=nullptr;
static constexpr wchar_t ClassName[]=L"GameWindowMapper.Canvas.v1";
static constexpr UINT FinishClose=WM_APP+71;
static constexpr UINT StatusChanged=WM_APP+72;
static constexpr UINT ActivateSource=WM_APP+73;
namespace {
struct StatusCard {
    std::wstring title,message;
    bool visible{},progress{},retry{},closing{};
};
StatusCard statusCard(const vd::CanvasStatus& status) {
    using vd::ipc::State;
    StatusCard card;
    if(status.finished)return card;
    card.visible=true;card.message=status.reply.message;card.closing=status.closing;
    if(status.closing){
        card.title=status.reply.state==State::RestoreFailed?L"源窗口恢复尚未完成":L"正在恢复源窗口";
        card.message=L"剩余 "+std::to_wstring(status.reply.remaining)+L" 个窗口。"+card.message;
        card.retry=status.reply.state==State::RestoreFailed;card.progress=!card.retry;return card;
    }
    switch(status.reply.state){
    case State::Checking:card.title=L"正在检查虚拟显示器";card.progress=true;break;
    case State::Installing:card.title=L"正在安装虚拟显示驱动";card.progress=true;break;
    case State::Initializing:card.title=L"正在初始化扩展屏";card.progress=true;break;
    case State::Restoring:card.title=L"正在恢复源窗口";card.progress=true;break;
    case State::RestoreFailed:card.title=L"源窗口恢复尚未完成";card.retry=true;break;
    case State::RestartRequired:card.title=L"驱动需要重启 Windows";card.retry=true;break;
    case State::Conflict:card.title=L"虚拟显示驱动被其他软件占用";card.retry=true;break;
    case State::Unavailable:card.title=L"隐藏功能暂不可用";card.retry=true;break;
    default:
        if(!status.transient.empty()&&nowMs()<status.transientUntil){card.title=L"窗口操作未完成";card.message=status.transient;}
        else card.visible=false;
    }
    if(card.message.empty())card.message=card.progress?L"普通窗口映射继续可用，请稍候。":L"普通窗口映射继续可用；可重新检查。";
    return card;
}
RECT cardRect(int w,int h,float dpi) {
    const int margin=std::min(int(16*dpi),std::max(0,w/8));
    const int width=std::max(1,std::min(w-2*margin,int(520*dpi)));
    const int top=std::min(int(12*dpi),std::max(0,h/8));
    return {(w-width)/2,top,(w+width)/2,std::min(h,top+int(172*dpi))};
}
}
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
void Engine::wait() {if(uiThread.joinable())uiThread.join();}
void Engine::stop() {requestStop();wait();state=GWM_STOPPED;}
Config Engine::snapshot(){std::lock_guard lock(configMutex);return config;}
Hit Engine::targetAt(int px,int py) {
    Hit target{};bool found=false;
    {
        std::lock_guard lock(configMutex);
        for(const auto& h:hits)if(PtInRect(&h.rect,{px,py})){
            if(h.slot<config.slots.size()&&config.slots[h.slot].generation==h.generation&&config.slots[h.slot].state!=GWM_INVALID){target=h;found=true;}
            break;
        }
    }
    return found?target:Hit{};
}
void Engine::activate(int px,int py) {
    const auto target=targetAt(px,py);
    if(!target.hwnd||controller.snapshot().closing)return;
    if(controllerStarted)controller.show(target.hwnd,true);
    else activateWindow(target.hwnd);
}
void Engine::activateWindow(uint64_t raw) {
    if(controller.snapshot().closing)return;
    {std::lock_guard lock(configMutex);if(std::none_of(config.slots.begin(),config.slots.end(),[&](const Slot& s){return s.hwnd==raw&&s.state!=GWM_INVALID;}))return;}
    HWND h=toWindow(static_cast<uint32_t>(raw));if(!IsWindow(h))return;
    if(IsIconic(h))ShowWindowAsync(h,SW_RESTORE);
    bool activated=SetForegroundWindow(h)!=FALSE;
    if(!activated){FLASHWINFO info{sizeof(info),h,FLASHW_TRAY,2,0};FlashWindowEx(&info);}
    std::lock_guard lock(configMutex);
    for(auto& slot:config.slots)if(slot.hwnd==raw&&slot.state!=GWM_INVALID)
        slot.error=activated?GWM_OK:GWM_ACTIVATION_DENIED;
}
void Engine::closeCanvas() {
    if(controllerStarted)controller.close();else requestStop();
}
void Engine::command(UINT id) {
    const auto status=controller.snapshot();
    if(id==vd::ui::Retry||id==vd::ui::RetryRestore){controller.retry();return;}
    if(status.closing||status.busy||!contextTarget.hwnd)return;
    bool valid=false;
    {std::lock_guard lock(configMutex);valid=contextTarget.slot<config.slots.size()&&
        config.slots[contextTarget.slot].generation==contextTarget.generation&&config.slots[contextTarget.slot].state!=GWM_INVALID;}
    if(!valid||!IsWindow(toWindow(contextTarget.hwnd)))return;
    if(id==vd::ui::Hide&&status.connected&&status.reply.state==vd::ipc::State::Ready&&status.reply.active&&!status.reply.statusError&&!status.hidden(contextTarget.hwnd))controller.hide(contextTarget.hwnd);
    if(id==vd::ui::Show&&status.hidden(contextTarget.hwnd))controller.show(contextTarget.hwnd);
}
void Engine::contextMenu(int sx,int sy) {
    HWND h=hwnd.load();POINT point{sx,sy};
    if(sx==-1&&sy==-1){
        RECT client{};GetClientRect(h,&client);point={client.right/2,client.bottom/2};
        if(!contextTarget.hwnd){std::lock_guard lock(configMutex);for(const auto& hit:hits)if(hit.hwnd){contextTarget=hit;break;}}
        ClientToScreen(h,&point);
    }else{POINT local=point;ScreenToClient(h,&local);contextTarget=targetAt(local.x,local.y);}
    const auto status=controller.snapshot();
    HMENU menu=CreatePopupMenu();if(!menu)return;
    const bool valid=contextTarget.hwnd&&IsWindow(toWindow(contextTarget.hwnd));
    if(valid){
        std::wstring label=L"槽位 "+std::to_wstring(contextTarget.slot+1)+L" · HWND "+std::to_wstring(contextTarget.hwnd);
        AppendMenuW(menu,MF_STRING|MF_GRAYED,0,label.c_str());AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool hidden=status.hidden(contextTarget.hwnd),free=!status.closing&&!status.busy;
        const bool ready=status.connected&&status.reply.state==vd::ipc::State::Ready&&status.reply.active&&!status.reply.statusError;
        AppendMenuW(menu,MF_STRING|(free&&ready&&!hidden?MF_ENABLED:MF_GRAYED),vd::ui::Hide,L"隐藏窗口");
        AppendMenuW(menu,MF_STRING|(free&&hidden?MF_ENABLED:MF_GRAYED),vd::ui::Show,L"显示窗口");
    }
    if(status.closing)AppendMenuW(menu,MF_STRING,vd::ui::RetryRestore,L"重试恢复");
    else if(statusCard(status).retry)AppendMenuW(menu,MF_STRING,vd::ui::Retry,L"重新检查");
    if(!GetMenuItemCount(menu)){DestroyMenu(menu);return;}
    const UINT selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,h,nullptr);
    DestroyMenu(menu);if(selected)command(selected);
}
void Engine::updateStatusControls() {
    if(!retryButton)return;
    BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);animateProgress=animations!=FALSE;
    RECT bounds{};GetClientRect(hwnd.load(),&bounds);const float dpi=GetDpiForWindow(hwnd.load())/96.0f;
    const auto card=statusCard(controller.snapshot());const auto r=cardRect(bounds.right,bounds.bottom,dpi);
    if(card.visible&&card.retry&&r.bottom-r.top>60*dpi){
        SetWindowTextW(retryButton,card.closing?L"重试恢复":L"重新检查");
        const int height=std::min(int(30*dpi),int(r.bottom-r.top)),width=std::min(int(120*dpi),int(r.right-r.left));
        SetWindowPos(retryButton,HWND_TOP,r.right-width-int(16*dpi),r.bottom-height-int(12*dpi),width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }else ShowWindow(retryButton,SW_HIDE);
}
LRESULT CALLBACK Engine::wndProc(HWND window,UINT message,WPARAM wp,LPARAM lp) noexcept {
    auto self=reinterpret_cast<Engine*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Engine*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(window,message,wp,lp);
    try{
        switch(message){
        case WM_CLOSE:self->closeCanvas();return 0;
        case FinishClose:DestroyWindow(window);return 0;
        case WM_DESTROY:self->requestStop();PostQuitMessage(0);return 0;
        case StatusChanged:self->updateStatusControls();self->wake.notify_all();return 0;
        case ActivateSource:self->activateWindow(static_cast<uint64_t>(wp));return 0;
        case WM_RBUTTONDOWN:self->contextTarget=self->targetAt(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
        case WM_CONTEXTMENU:self->contextMenu(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
        case WM_COMMAND:self->command(LOWORD(wp));return 0;
        case WM_KEYDOWN:if(wp==VK_F10&&(GetKeyState(VK_SHIFT)&0x8000)){self->contextMenu(-1,-1);return 0;}break;
        case WM_LBUTTONDBLCLK:self->activate(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
        case WM_SIZE:self->updateStatusControls();self->wake.notify_all();return 0;
        case WM_SETTINGCHANGE:self->updateStatusControls();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT p{};BeginPaint(window,&p);EndPaint(window,&p);return 0;}
        case WM_DPICHANGED:{const auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        case WM_GETMINMAXINFO:{auto p=reinterpret_cast<MINMAXINFO*>(lp);p->ptMinTrackSize={160,120};return 0;}
        }
    }catch(...){self->closeCanvas();}
    return DefWindowProcW(window,message,wp,lp);
}
void Engine::uiMain() noexcept {
    bool registered=false;
    HICON largeIcon=nullptr,smallIcon=nullptr;
    auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try{
        WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_DBLCLKS|CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=wndProc;
        wc.hInstance=module;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        largeIcon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,32,32,0));
        smallIcon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,16,16,0));
        wc.hIcon=largeIcon;wc.hIconSm=smallIcon;wc.lpszClassName=ClassName;
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
        // Do not inherit STARTUPINFO.wShowWindow from a hidden/tray host.
        SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);UpdateWindow(h);
        retryButton=CreateWindowExW(0,L"BUTTON",L"重新检查",WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,
            0,0,120,30,h,reinterpret_cast<HMENU>(vd::ui::Retry),module,nullptr);
        SendMessageW(retryButton,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        std::vector<uint64_t> windows;for(const auto& slot:config.slots)if(slot.hwnd)windows.push_back(slot.hwnd);
        controller.start(module,std::move(windows),[this]{PostMessageW(hwnd.load(),StatusChanged,0,0);},
            [this]{requestStop();},[this](uint64_t source){PostMessageW(hwnd.load(),ActivateSource,static_cast<WPARAM>(source),0);});
        controllerStarted=true;
        renderThread=std::thread([this]{renderMain();});
        {std::lock_guard lock(wakeMutex);uiReady=true;}wake.notify_all();
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0)if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }catch(...){
        requestStop();{std::lock_guard lock(wakeMutex);startupError=GWM_INTERNAL_ERROR;uiReady=true;if(!renderThread.joinable())renderReady=true;}wake.notify_all();
    }
    stopRequested=true;wake.notify_all();
    controller.shutdown();
    if(renderThread.joinable())renderThread.join();
    if(auto h=hwnd.exchange(nullptr);h&&IsWindow(h))DestroyWindow(h);
    if(registered)UnregisterClassW(ClassName,module);
    if(largeIcon)DestroyIcon(largeIcon);
    if(smallIcon)DestroyIcon(smallIcon);
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
    if(!graphics){stopRequested=true;PostMessageW(hwnd.load(),FinishClose,0,0);winrt::clear_factory_cache();if(SUCCEEDED(co))CoUninitialize();if(previous)SetThreadDpiAwarenessContext(previous);return;}
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
                const auto virtualStatus=controller.snapshot();
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
                        wchar_t buffer[512]{};InternalGetWindowText(toWindow(s->hwnd),buffer,512);
                        title+=buffer[0]?std::wstring(buffer):L"HWND "+std::to_wstring(s->hwnd);
                    }else title+=L"空槽位";
                    auto textBox=titleBox;textBox.left+=8; textBox.right-=6;
                    if(s&&virtualStatus.hidden(s->hwnd)){
                        const float badgeWidth=std::min(68*dpiScale,(titleBox.right-titleBox.left)*.5f);
                        auto badge=titleBox;badge.left=badge.right-badgeWidth;
                        graphics->text(L"虚拟屏",badge,0x85c6ff,true);textBox.right=badge.left-4*dpiScale;
                    }
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
                const auto card=statusCard(virtualStatus);
                if(card.visible){
                    const auto r=cardRect(bounds.right,bounds.bottom,dpiScale);
                    auto box=D2D1::RectF(float(r.left),float(r.top),float(r.right),float(r.bottom));
                    graphics->fill(box,0x353b46);box.left+=1;box.top+=1;box.right-=1;box.bottom-=1;graphics->fill(box,0x20252d);
                    auto text=box;text.left+=16*dpiScale;text.right-=16*dpiScale;text.top+=8*dpiScale;text.bottom=std::min(box.bottom,text.top+30*dpiScale);
                    graphics->text(card.title,text,0xe6eaf0);
                    text.top=text.bottom+4*dpiScale;text.bottom=std::min(box.bottom-48*dpiScale,text.top+72*dpiScale);
                    if(text.bottom>text.top)graphics->text(card.message,text,0xadb5c3,true);
                    if(card.progress&&box.bottom-box.top>80*dpiScale){
                        auto bar=D2D1::RectF(box.left+16*dpiScale,box.bottom-24*dpiScale,box.right-16*dpiScale,box.bottom-20*dpiScale);
                        graphics->fill(bar,0x353b46);const float width=bar.right-bar.left;
                        const float phase=animateProgress?float(nowMs()%1600)/1600.0f:.5f;
                        const float length=width*.24f;bar.left+=(width-length)*phase;bar.right=bar.left+length;graphics->fill(bar,0x85c6ff);
                    }
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
    // Factory pointers cached by C++/WinRT must not outlive this COM apartment:
    // its final CoUninitialize can unload the capture implementation while our
    // DLL remains loaded, leaving IsSupported's cached vtable stale on restart.
    winrt::clear_factory_cache();
    if(SUCCEEDED(co))CoUninitialize();if(previous)SetThreadDpiAwarenessContext(previous);
}
}
