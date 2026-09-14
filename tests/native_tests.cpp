#include "GameWindowMapper.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
bool until(const std::function<bool()>& f,int ms=8000){auto end=GetTickCount64()+ms;do{pump();if(f())return true;Sleep(20);}while(GetTickCount64()<end);return false;}
LRESULT CALLBACK sourceProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_PAINT){PAINTSTRUCT p;auto dc=BeginPaint(h,&p);RECT r;GetClientRect(h,&r);
        auto brush=CreateSolidBrush(GetWindowLongPtrW(h,GWLP_USERDATA)?RGB(32,192,64):RGB(224,48,32));
        FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(h,&p);return 0;}
    return DefWindowProcW(h,m,w,l);
}
HWND canvas(){
    HWND h=nullptr;while((h=FindWindowExW(nullptr,h,L"GameWindowMapper.Canvas.v1",nullptr))){DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())return h;}return nullptr;
}
bool color(HWND h,int x,int y,COLORREF expected){
    POINT point{x,y};ClientToScreen(h,&point);auto dc=GetDC(nullptr);auto actual=GetPixel(dc,point.x,point.y);ReleaseDC(nullptr,dc);
    return actual!=CLR_INVALID&&abs(int(GetRValue(actual))-GetRValue(expected))<8&&
        abs(int(GetGValue(actual))-GetGValue(expected))<8&&abs(int(GetBValue(actual))-GetBValue(expected))<8;
}
void integer(std::vector<uint8_t>& b,int32_t n){auto p=reinterpret_cast<uint8_t*>(&n);b.insert(b.end(),p,p+4);}
std::vector<uint8_t> background(){
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+16;
    info.biSize=sizeof(info);info.biWidth=2;info.biHeight=2;info.biPlanes=1;info.biBitCount=24;info.biSizeImage=16;
    std::vector<uint8_t> b(file.bfSize);
    std::memcpy(b.data(),&file,sizeof(file));std::memcpy(b.data()+sizeof(file),&info,sizeof(info));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){auto p=b.data()+file.bfOffBits+y*8+x*3;p[0]=192;p[1]=64;p[2]=32;}
    return b;
}
std::vector<uint8_t> config(){
    auto bmp=background();std::vector<uint8_t> b;
    for(int n:{1,30,int(bmp.size()),2})integer(b,n);b.insert(b.end(),bmp.begin(),bmp.end());
    // CP936 title, then UTF-16 title + ROI + forced global background.
    for(int n:{1,0,0,0,0,4,0,0})integer(b,n);
    for(int n:{0xb1,0xea,0xcc,0xe2})b.push_back(static_cast<uint8_t>(n));
    for(int n:{4,10,10,100,100,4,0,7})integer(b,n);
    for(int n:{0x4f,0x5c,0x55,0x5e})b.push_back(static_cast<uint8_t>(n));
    return b;
}
void exports(HMODULE dll){
    auto base=reinterpret_cast<const uint8_t*>(dll);
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    require(nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386,"DLL must be x86");
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto entry=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+directory.VirtualAddress);
    require(entry->NumberOfNames==1&&entry->NumberOfFunctions==1,"exactly one exported function");
    require(GetProcAddress(dll,"WM_StartMapping")!=nullptr,"fixed undecorated export");
    require(GetProcAddress(dll,"WM_Stop")==nullptr,"no Stop export");
}
}

int wmain(int argc,wchar_t** argv){
    std::cout.setf(std::ios::unitbuf);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::wstring path;
    if(argc>1)path=argv[1];else {wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);path=exe;path.resize(path.find_last_of(L"\\/")+1);path+=L"GameWindowMapper.dll";}
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=sourceProc;wc.lpszClassName=L"GWM.SingleEntry.TestSource";
    RegisterClassW(&wc);
    HWND a=CreateWindowW(wc.lpszClassName,L"本轮源 A",WS_OVERLAPPEDWINDOW,20,20,260,200,nullptr,nullptr,wc.hInstance,nullptr);
    HWND b=CreateWindowW(wc.lpszClassName,L"本轮源 B",WS_OVERLAPPEDWINDOW,310,20,260,200,nullptr,nullptr,wc.hInstance,nullptr);
    SetWindowLongPtrW(b,GWLP_USERDATA,1);ShowWindow(a,SW_SHOW);ShowWindow(b,SW_SHOW);UpdateWindow(a);UpdateWindow(b);
    HMODULE dll=nullptr;
    try{
        for(int cycle=0;cycle<3;++cycle){
            if(!dll)dll=LoadLibraryW(path.c_str());require(dll!=nullptr,"load DLL");exports(dll);
            auto start=reinterpret_cast<decltype(&WM_StartMapping)>(GetProcAddress(dll,"WM_StartMapping"));
            uint32_t handles[]{uint32_t(uintptr_t(b)),uint32_t(uintptr_t(a)),0,uint32_t(uintptr_t(b))};
            auto settings=config();
            if(cycle==0){
                require(start(handles,4,2,2,80,300,640,480,0,0,200,120,settings.data(),15)==GWM_INVALID_ARGUMENT,"truncated config rejected");
                require(start(handles,4,1,1,80,300,640,480,0,0,200,120,nullptr,0)==GWM_CAPACITY_EXCEEDED,"capacity rejected");
                auto broken=settings;broken[16]=0;
                require(start(handles,4,2,2,80,300,640,480,0,0,200,120,broken.data(),int(broken.size()))==GWM_IMAGE_DECODE_FAILED,"bad BMP rejected");
            }
            // Last cycle exercises the empty-config/default path.
            int started=GWM_ALREADY_STARTED;
            require(until([&]{started=start(handles,4,2,2,80,300,640,480,0,0,200,120,
                cycle==2?nullptr:settings.data(),cycle==2?0:int(settings.size()));return started!=GWM_ALREADY_STARTED;})&&started==GWM_OK,"start or restart without reloading DLL");
            require(start(handles,4,2,2,80,300,640,480,0,0,200,120,nullptr,0)==GWM_ALREADY_STARTED,"duplicate start");
            std::fill(std::begin(handles),std::end(handles),0);settings.clear();settings.shrink_to_fit();
            require(until([]{auto h=canvas();return h&&IsWindowVisible(h);}),"canvas visible");
            auto h=canvas();
            bool mapped=until([&]{return color(h,160,100,RGB(32,192,64))&&color(h,480,100,RGB(224,48,32));}); if(!mapped){POINT p{160,100};ClientToScreen(h,&p);auto dc=GetDC(nullptr);std::cerr<<"screen pixel="<<GetPixel(dc,p.x,p.y)<<" at "<<p.x<<","<<p.y<<" source="<<GetPixel(dc,80,100)<<" lastError="<<GetLastError()<<"\n";ReleaseDC(nullptr,dc);} require(mapped,"WGC pixels preserve copied array order");
            if(cycle!=2)require(until([&]{return color(h,480,320,RGB(32,64,192));}),"copied config forced BMP fallback");
            else require(until([&]{return color(h,480,340,RGB(32,192,64));}),"duplicate HWND with default config");
            // Drop host reference BEFORE close. DLL must stay alive until its
            // own threads and resources are gone, then unload without any API.
            // First cycle keeps the host reference to exercise re-starting the
            // SAME loaded module; later cycles verify early host unload safety.
            if(cycle!=0){FreeLibrary(dll);dll=nullptr;}
            require(GetModuleHandleW(path.c_str())!=nullptr,"active session holds module reference");
            PostMessageW(h,WM_CLOSE,0,0);
            require(until([]{return !canvas();}),"canvas destroyed");
            if(cycle!=0)require(until([&]{return GetModuleHandleW(path.c_str())==nullptr;}),"automatic teardown and DLL self-reference released");
            require(IsWindow(a)&&IsWindow(b),"source windows survive");
            std::cout<<"cycle "<<cycle+1<<": display, duplicate start, close, "<<(cycle==0?"host keeps DLL loaded":"automatic unload")<<" OK\n";
        }
        std::cout<<"PASS "<<checks<<" checks; three close/reopen cycles, no Stop call\n";
    }catch(const std::exception& e){
        if(auto h=canvas()){PostMessageW(h,WM_CLOSE,0,0);until([]{return !canvas();});}
        if(dll)FreeLibrary(dll);
        DestroyWindow(a);DestroyWindow(b);UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;
    }
    DestroyWindow(a);DestroyWindow(b);UnregisterClassW(wc.lpszClassName,wc.hInstance);return 0;
}
