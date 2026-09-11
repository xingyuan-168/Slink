#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "wgc_probe.h"

namespace {
constexpr UINT moveFlags=SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOSENDCHANGING;
struct Baseline{RECT window{},client{};LONG_PTR style{},exStyle{};UINT dpi{};WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};};
Baseline Capture(HWND hwnd){Baseline b;GetWindowRect(hwnd,&b.window);GetClientRect(hwnd,&b.client);b.style=GetWindowLongPtrW(hwnd,GWL_STYLE);b.exStyle=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);b.dpi=GetDpiForWindow(hwnd);GetWindowPlacement(hwnd,&b.placement);return b;}
void PrintBaseline(const char*phase,const Baseline&b){std::cout<<phase<<" outer="<<b.window.left<<","<<b.window.top<<","<<b.window.right-b.window.left<<"x"<<b.window.bottom-b.window.top<<" client="<<b.client.right<<"x"<<b.client.bottom<<" style="<<std::hex<<b.style<<" exStyle="<<b.exStyle<<std::dec<<" dpi="<<b.dpi<<" showCmd="<<b.placement.showCmd<<std::endl;}
bool Invariants(const Baseline&a,const Baseline&b){return a.window.right-a.window.left==b.window.right-b.window.left&&a.window.bottom-a.window.top==b.window.bottom-b.window.top&&EqualRect(&a.client,&b.client)&&a.style==b.style&&a.exStyle==b.exStyle&&a.dpi==b.dpi&&a.placement.showCmd==b.placement.showCmd;}
bool IsOffscreen(HWND hwnd){RECT r{},desktop{},overlap{};if(!GetWindowRect(hwnd,&r))return false;desktop.left=GetSystemMetrics(SM_XVIRTUALSCREEN);desktop.top=GetSystemMetrics(SM_YVIRTUALSCREEN);desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);return !IntersectRect(&overlap,&r,&desktop);}
bool Pump(DWORD ms){const ULONGLONG end=GetTickCount64()+ms;while(GetTickCount64()<end){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)return false;TranslateMessage(&msg);DispatchMessageW(&msg);}MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);}return true;}
HTHUMBNAIL thumbnail{};HWND canvas{},source{};Baseline baseline{};bool parked{};WgcProbe* wgc{};
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_APP+1&&wgc){wgc->Update();return 0;}
    if(msg==WM_SIZE&&thumbnail){RECT r{};GetClientRect(hwnd,&r);DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION;p.rcDestination=r;DwmUpdateThumbnailProperties(thumbnail,&p);return 0;}
    if(msg==WM_CLOSE){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,msg,wp,lp);
}
struct Child{
    HANDLE process{};DWORD id{};
    ~Child(){if(process){if(IsWindow(source))PostMessageW(source,WM_CLOSE,0,0);if(WaitForSingleObject(process,2000)!=WAIT_OBJECT_0){TerminateProcess(process,9);WaitForSingleObject(process,2000);}CloseHandle(process);}}
};
BOOL CALLBACK FindSource(HWND hwnd,LPARAM ptr){DWORD pid{};GetWindowThreadProcessId(hwnd,&pid);if(pid==*reinterpret_cast<DWORD*>(ptr)){wchar_t cls[64]{};GetClassNameW(hwnd,cls,64);if(std::wstring(cls)==L"SLinkTestSource"){source=hwnd;return FALSE;}}return TRUE;}
bool Launch(Child&child,const std::wstring&kind){wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);std::wstring exe(path);exe=exe.substr(0,exe.find_last_of(L"\\/")+1)+L"test_source.exe";std::wstring command=L"\""+exe+L"\" "+kind;STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))return false;child.process=pi.hProcess;child.id=pi.dwProcessId;CloseHandle(pi.hThread);for(int i=0;i<150&&!source;++i){EnumWindows(FindSource,reinterpret_cast<LPARAM>(&child.id));Pump(20);}return source!=nullptr;}
// Test-only desktop sampling: read this process's visible PoC canvas, never the production mapping path.
int ReadFrame(){
    RECT client{};GetClientRect(canvas,&client);if(client.right<=0||client.bottom<=0)return -1;
    POINT pt{};ClientToScreen(canvas,&pt);HDC screen=GetDC(nullptr);if(!screen)return -1;
    HDC memory=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=client.right;info.bmiHeader.biHeight=-1;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* raw{};HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&raw,nullptr,0);
    if(!memory||!bitmap){if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);ReleaseDC(nullptr,screen);return -1;}
    HGDIOBJ old=SelectObject(memory,bitmap);
    bool valid=BitBlt(memory,0,0,client.right,1,screen,pt.x,pt.y+static_cast<int>(24*static_cast<double>(client.bottom)/600),SRCCOPY|CAPTUREBLT)!=FALSE;
    GdiFlush();unsigned value=0;auto pixels=static_cast<const uint8_t*>(raw);
    for(int bit=0;valid&&bit<20;++bit){int x=static_cast<int>((bit*24+12)*static_cast<double>(client.right)/800);const uint8_t* pixel=pixels+x*4;
        bool on=pixel[2]>180&&pixel[0]<80,off=pixel[0]>180&&pixel[2]<80;
        if(!on&&!off){valid=false;break;}if(bit<4){if(on!=(((0xAu>>bit)&1u)!=0))valid=false;}else if(on)value|=1u<<(bit-4);
    }
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);
    return valid?static_cast<int>(value):-1;
}
bool NewFrames(int seconds,const char*phase){int previous=-1,changes=0,valid=0;const ULONGLONG end=GetTickCount64()+static_cast<ULONGLONG>(seconds)*1000;while(GetTickCount64()<end){if(!Pump(60))return false;DwmFlush();int f=ReadFrame();if(f>=0){++valid;if(previous>=0&&previous!=f)++changes;previous=f;}}std::cout<<phase<<": valid_samples="<<valid<<" frame_changes="<<changes<<" last_frame="<<previous<<" fixture_timer="<<reinterpret_cast<UINT_PTR>(GetPropW(source,L"SLinkFixtureFrame"))<<" fixture_paint="<<reinterpret_cast<UINT_PTR>(GetPropW(source,L"SLinkFixturePaint"))<<" present_hr="<<std::hex<<reinterpret_cast<UINT_PTR>(GetPropW(source,L"SLinkFixturePresent"))<<std::dec<<" wgc_frames="<<(wgc?wgc->frames:0)<<" wgc_hr="<<std::hex<<(wgc?static_cast<unsigned>(wgc->error):0u)<<std::dec<<std::endl;return valid>2&&changes>2;}
bool MoveOnly(int x,int y){return SetWindowPos(source,nullptr,x,y,0,0,moveFlags)!=FALSE;}
bool Restore(){if(!parked)return true;if(!IsWindow(source))return false;bool ok=MoveOnly(baseline.window.left,baseline.window.top);Pump(100);auto after=Capture(source);ok=ok&&Invariants(baseline,after)&&after.window.left==baseline.window.left&&after.window.top==baseline.window.top;if(ok)parked=false;return ok;}
struct Cleanup{~Cleanup(){Restore();if(thumbnail)DwmUnregisterThumbnail(thumbnail);thumbnail=nullptr;if(canvas)DestroyWindow(canvas);}};
}
int wmain(int argc,wchar_t**argv){
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::wstring kind=L"gdi",edge=L"right";int cycles=2,seconds=2;bool maximize=false,useWgc=false,geometryOnly=false;
    for(int i=1;i<argc;++i){std::wstring arg=argv[i];if(arg==L"--backend"&&i+1<argc)kind=argv[++i];else if(arg==L"--cycles"&&i+1<argc)cycles=_wtoi(argv[++i]);else if(arg==L"--seconds"&&i+1<argc)seconds=_wtoi(argv[++i]);else if(arg==L"--edge"&&i+1<argc)edge=argv[++i];else if(arg==L"--maximized")maximize=true;else if(arg==L"--wgc")useWgc=true;else if(arg==L"--geometry-only")geometryOnly=true;else{std::cerr<<"Unknown argument"<<std::endl;return 2;}}
    if((kind!=L"gdi"&&kind!=L"d3d")||cycles<1||seconds<1||(edge!=L"right"&&edge!=L"left"&&edge!=L"top"&&edge!=L"bottom"))return 2;
    Child child; if(!Launch(child,kind)){std::cerr<<"Fixture launch failed: "<<GetLastError()<<std::endl;return 3;}
    if(maximize){ShowWindow(source,SW_MAXIMIZE);Pump(200);}
    Pump(500);baseline=Capture(source);PrintBaseline("baseline",baseline);WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SLinkDwmPoc";wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));RegisterClassW(&wc);
    RECT r{0,0,800,600};AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_TOPMOST);
    canvas=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"CQ_SLink DWM compatibility test",WS_OVERLAPPEDWINDOW,60,60,r.right-r.left,r.bottom-r.top,nullptr,nullptr,wc.hInstance,nullptr);
    Cleanup cleanup;if(!canvas){std::cerr<<"Canvas create failed"<<std::endl;return 4;}
    ShowWindow(canvas,SW_SHOWNOACTIVATE);WgcProbe capture;struct ProbeGuard{~ProbeGuard(){wgc=nullptr;}} guard;HRESULT hr{};
    if(useWgc){try{wgc=&capture;capture.Open(canvas,source);}catch(winrt::hresult_error const&e){std::cerr<<"WGC init failed "<<std::hex<<e.code().value<<std::endl;return 5;}}
    else {hr=DwmRegisterThumbnail(canvas,source,&thumbnail);if(FAILED(hr)){std::cerr<<"Register failed "<<std::hex<<hr<<std::endl;return 5;}
    DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_RECTSOURCE|DWM_TNP_VISIBLE|DWM_TNP_OPACITY|DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcSource={0,0,800,600};p.rcDestination={0,0,800,600};p.fVisible=TRUE;p.opacity=255;p.fSourceClientAreaOnly=TRUE;
    hr=DwmUpdateThumbnailProperties(thumbnail,&p);if(FAILED(hr))return 6;}
    Pump(300);std::cout<<"backend="<<(kind==L"gdi"?"gdi":"d3d")<<" maximized="<<maximize<<" client="<<baseline.client.right<<"x"<<baseline.client.bottom<<" dpi="<<baseline.dpi<<std::endl;
    if(!geometryOnly&&!NewFrames(seconds,"visible_occluded")){std::cerr<<"FAIL visible canvas frame verification"<<std::endl;return 7;}
    int parkX=GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+64;
    int parkY=GetSystemMetrics(SM_YVIRTUALSCREEN)+64;
    if(edge==L"left")parkX=GetSystemMetrics(SM_XVIRTUALSCREEN)-(baseline.window.right-baseline.window.left)-64;
    if(edge==L"top"||edge==L"bottom"){parkX=GetSystemMetrics(SM_XVIRTUALSCREEN)+64;parkY=edge==L"top"?GetSystemMetrics(SM_YVIRTUALSCREEN)-(baseline.window.bottom-baseline.window.top)-64:GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)+64;}
    std::cout<<"park_position="<<parkX<<","<<parkY<<" geometry_only="<<geometryOnly<<std::endl;
    for(int i=0;i<cycles;++i){parked=true;if(!MoveOnly(parkX,parkY)||!Pump(100)||!Invariants(baseline,Capture(source))||!IsOffscreen(source)){PrintBaseline("after",Capture(source));std::cerr<<"FAIL invariant or position move cycle="<<i<<std::endl;Restore();return 8;}if(!geometryOnly&&!NewFrames(seconds,"parked")){std::cerr<<"FAIL parked frame continuity; restoring source"<<std::endl;bool recovered=Restore();std::cout<<"rollback="<<recovered<<std::endl;if(recovered)NewFrames(seconds,"after_failed_hide_restored");return 9;}if(!Restore()){std::cerr<<"FAIL rollback"<<std::endl;return 10;}}
    if(!geometryOnly&&!NewFrames(seconds,"restored"))return 11;
    std::cout<<"PASS: "<<(geometryOnly?"geometry only (frame continuity NOT tested)":useWgc?"WGC frame continuity":"DWM frame continuity")<<" and "<<cycles<<" zero-drift cycles"<<std::endl;return 0;
}
