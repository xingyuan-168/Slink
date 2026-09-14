#include "GameWindowMapper.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks=0,pixelChecksSkipped=0;
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
bool desktopReadable(){
    auto dc=GetDC(nullptr);if(!dc)return false;
    auto pixel=GetPixel(dc,0,0);ReleaseDC(nullptr,dc);return pixel!=CLR_INVALID;
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
std::vector<uint8_t> pngBackground(){
    // Complete 2x2 RGBA PNG, RGB(32,64,192); same color as the BMP fixture.
    return {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x06,0x00,0x00,0x00,0x72,0xb6,0x0d,0x24,0x00,0x00,0x00,0x01,0x73,0x52,0x47,0x42,0x00,0xae,0xce,0x1c,0xe9,0x00,0x00,0x00,0x04,0x67,0x41,0x4d,0x41,0x00,0x00,0xb1,0x8f,0x0b,0xfc,0x61,0x05,0x00,0x00,0x00,0x09,0x70,0x48,0x59,0x73,0x00,0x00,0x0e,0xc3,0x00,0x00,0x0e,0xc3,0x01,0xc7,0x6f,0xa8,0x64,0x00,0x00,0x00,0x11,0x49,0x44,0x41,0x54,0x18,0x57,0x63,0x50,0x70,0x38,0xf0,0x1f,0x84,0x19,0x60,0x0c,0x00,0x46,0x34,0x08,0x7d,0xe9,0xe3,0x67,0xa8,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
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
        std::ifstream jpg(std::filesystem::u8path(__FILE__).parent_path().parent_path()/L"examples"/L"黑.jpg",std::ios::binary);
        std::vector<uint8_t> userPicture((std::istreambuf_iterator<char>(jpg)),std::istreambuf_iterator<char>());
        require(!userPicture.empty(),"read user JPEG regression fixture");
        for(int cycle=0;cycle<4;++cycle){
            if(!dll)dll=LoadLibraryW(path.c_str());require(dll!=nullptr,"load DLL");exports(dll);
            auto start=reinterpret_cast<decltype(&WM_StartMapping)>(GetProcAddress(dll,"WM_StartMapping"));
            uint32_t handles[]{uint32_t(uintptr_t(b)),uint32_t(uintptr_t(a)),cycle==0?0u:0xffffffffu,uint32_t(uintptr_t(b))};
            auto settings=cycle==0?config():(cycle==1?background():cycle==2?pngBackground():userPicture);
            if(cycle==0){
                require(start(handles,4,2,2,80,300,640,480,0,0,200,120,settings.data(),15)==GWM_INVALID_ARGUMENT,"truncated config rejected");
                require(start(handles,4,1,1,80,300,640,480,0,0,200,120,nullptr,0)==GWM_CAPACITY_EXCEEDED,"capacity rejected");
                auto broken=settings;broken[16]=0;
                require(start(handles,4,2,2,80,300,640,480,0,0,200,120,broken.data(),int(broken.size()))==GWM_IMAGE_DECODE_FAILED,"bad BMP rejected");
            }
            // Same host: packed config -> raw BMP -> raw PNG -> user's JPEG.
            int started=GWM_ALREADY_STARTED;
            require(until([&]{started=start(handles,4,2,2,80,300,640,480,0,0,200,120,
                settings.data(),int(settings.size()));return started!=GWM_ALREADY_STARTED;})&&started==GWM_OK,"start or restart without reloading DLL");
            require(start(handles,4,2,2,80,300,640,480,0,0,200,120,nullptr,0)==GWM_ALREADY_STARTED,"duplicate start");
            std::fill(std::begin(handles),std::end(handles),0);settings.clear();settings.shrink_to_fit();
            require(until([]{auto h=canvas();return h&&IsWindowVisible(h);}),"canvas visible");
            auto h=canvas();
            if(desktopReadable()){
            bool mapped=until([&]{return color(h,160,100,RGB(32,192,64))&&color(h,480,100,RGB(224,48,32));}); if(!mapped){POINT p{160,100};ClientToScreen(h,&p);auto dc=GetDC(nullptr);std::cerr<<"screen pixel="<<GetPixel(dc,p.x,p.y)<<" at "<<p.x<<","<<p.y<<" source="<<GetPixel(dc,80,100)<<" lastError="<<GetLastError()<<"\n";ReleaseDC(nullptr,dc);} require(mapped,"WGC pixels preserve copied array order");
            if(cycle==0)require(until([&]{return color(h,480,320,RGB(32,64,192));}),"copied config forced BMP fallback");
            else {
                require(until([&]{return color(h,480,340,RGB(32,192,64));}),"raw image does not force live HWND to background");
                if(cycle<3)require(until([&]{return color(h,160,320,RGB(32,64,192));}),"raw BMP/PNG fallback for invalid HWND");
            }
            }else{++pixelChecksSkipped;std::cout<<"NOT VERIFIED: desktop pixel access unavailable for cycle "<<cycle+1<<"\n";}
            // Drop host reference BEFORE close. DLL must stay alive until its
            // own threads and resources are gone, then unload without any API.
            // Keep the DLL loaded for both restarts, as E-language does.
            if(cycle==3){FreeLibrary(dll);dll=nullptr;}
            require(GetModuleHandleW(path.c_str())!=nullptr,"active session holds module reference");
            PostMessageW(h,WM_CLOSE,0,0);
            require(until([]{return !canvas();}),"canvas destroyed");
            if(cycle==3)require(until([&]{return GetModuleHandleW(path.c_str())==nullptr;}),"automatic teardown and DLL self-reference released");
            require(IsWindow(a)&&IsWindow(b),"source windows survive");
            std::cout<<"cycle "<<cycle+1<<": "<<(cycle==0?"packed config":cycle==1?"raw BMP":cycle==2?"raw PNG":"user JPEG")<<", canvas created, close, "<<(cycle<3?"host keeps DLL loaded":"automatic unload")<<" OK\n";
        }
        std::cout<<"PASS "<<checks<<" lifecycle/config checks; four close/reopen cycles, no Stop call; pixel cycles skipped="<<pixelChecksSkipped<<"\n";
    }catch(const std::exception& e){
        if(auto h=canvas()){PostMessageW(h,WM_CLOSE,0,0);until([]{return !canvas();});}
        if(dll)FreeLibrary(dll);
        DestroyWindow(a);DestroyWindow(b);UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;
    }
    DestroyWindow(a);DestroyWindow(b);UnregisterClassW(wc.lpszClassName,wc.hInstance);return 0;
}
