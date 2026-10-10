#include "recovery_prompt.h"
#include <algorithm>

namespace gwm::vd {
namespace { constexpr UINT Update = WM_APP+81; constexpr wchar_t ClassName[]=L"GameWindowMapper.Recovery.v1"; }
RecoveryPrompt::~RecoveryPrompt() {
    if(thread_.joinable()){WaitForSingleObject(ready_,5000);PostThreadMessageW(threadId_,WM_QUIT,0,0);thread_.join();}
    if(ready_)CloseHandle(ready_);
}
void RecoveryPrompt::update(unsigned remaining,DWORD error,const std::wstring& message) {
    {std::scoped_lock lock(mutex_);if(remaining_==remaining&&error_==error&&message_==message)return;remaining_=remaining;error_=error;message_=message;}
    if(!thread_.joinable()&&(remaining||error)){thread_=std::thread([this]{run();});WaitForSingleObject(ready_,5000);}
    if(thread_.joinable())PostThreadMessageW(threadId_,Update,0,0);
}
void RecoveryPrompt::refresh(HWND window) {
    if(!text_||!button_)return;
    unsigned remaining{};DWORD error{};std::wstring message;
    {std::scoped_lock lock(mutex_);remaining=remaining_;error=error_;message=message_;}
    if(!remaining&&!error){ShowWindow(window,SW_HIDE);return;}
    std::wstring content=L"宿主已退出，仍有 "+std::to_wstring(remaining)+L" 个窗口等待恢复。\r\n"+message+
        L"\r\n错误码："+std::to_wstring(error)+L"。恢复记录和虚拟屏将保留，程序会继续重试。";
    SetWindowTextW(text_,content.c_str());
    RECT bounds{};GetClientRect(window,&bounds);const UINT dpi=GetDpiForWindow(window);const int gap=MulDiv(16,dpi,96),height=MulDiv(32,dpi,96);
    SetWindowPos(text_,nullptr,gap,gap,std::max(1L,bounds.right-gap*2),std::max(1L,bounds.bottom-height-gap*3),SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(button_,nullptr,gap,std::max(gap,int(bounds.bottom)-height-gap),MulDiv(120,dpi,96),height,SWP_NOZORDER|SWP_NOACTIVATE);
    if(!font_)font_=CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    SendMessageW(text_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);SendMessageW(button_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    if(!IsWindowVisible(window))ShowWindow(window,SW_SHOWNOACTIVATE);
}
LRESULT CALLBACK RecoveryPrompt::proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto self=reinterpret_cast<RecoveryPrompt*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<RecoveryPrompt*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(self)switch(message){
    case WM_COMMAND:if(LOWORD(w)==1&&self->retry_)self->retry_();return 0;
    case WM_CLOSE:ShowWindow(window,SW_MINIMIZE);return 0;
    case WM_SIZE:self->refresh(window);return 0;
    case WM_DPICHANGED:{const auto r=reinterpret_cast<RECT*>(l);if(self->font_){DeleteObject(self->font_);self->font_=nullptr;}SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    }
    return DefWindowProcW(window,message,w,l);
}
void RecoveryPrompt::run() {
    threadId_=GetCurrentThreadId();MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    const auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=ClassName;
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_APPWINDOW,ClassName,L"GameWindowMapper · 窗口恢复",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,520,230,nullptr,nullptr,instance,this);
    if(window){
        text_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,instance,nullptr);
        button_=CreateWindowW(L"BUTTON",L"重试恢复",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(1),instance,nullptr);
    }
    SetEvent(ready_);
    if(window){
        refresh(window);
        while(GetMessageW(&message,nullptr,0,0)>0){
            if(message.message==Update&&!message.hwnd)refresh(window);
            else if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
        DestroyWindow(window);
    }
    if(font_){DeleteObject(font_);font_=nullptr;}
    UnregisterClassW(ClassName,instance);
    if(previous)SetThreadDpiAwarenessContext(previous);
}
}
