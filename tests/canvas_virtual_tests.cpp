#include "GameWindowMapper.h"
#include "capture.h"
#include "virtual/client.h"
#include "virtual/ui_ids.h"
#include "virtual_test_source.h"
#include "virtual_test_guard.h"
#include <array>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>

namespace {
int checks{};
void require(bool pass,const char* name){++checks;std::printf("%s %s\n",pass?"PASS":"FAIL",name);std::fflush(stdout);if(!pass)throw std::runtime_error(name);}
bool until(const std::function<bool()>& predicate,DWORD ms=10000){const auto end=GetTickCount64()+ms;do{if(predicate())return true;Sleep(30);}while(GetTickCount64()<end);return predicate();}
HWND canvas(){HWND h{};while((h=FindWindowExW(nullptr,h,L"GameWindowMapper.Canvas.v1",nullptr))){DWORD pid{};GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())return h;}return nullptr;}
bool at(HWND h,const RECT& expected){RECT rect{};return GetWindowRect(h,&rect)&&EqualRect(&rect,&expected);}
bool hidden(HWND h){const auto display=gwm::vd::InspectOwnedDisplay();RECT rect{},intersection{};return display.active&&GetWindowRect(h,&rect)&&IntersectRect(&intersection,&rect,&display.bounds)&&EqualRect(&intersection,&rect);}
void command(int slot,UINT action){const auto h=canvas();RECT rect{};GetClientRect(h,&rect);const int x=(slot%2)*rect.right/2+rect.right/4,y=(slot/2)*rect.bottom/2+rect.bottom/4;SendMessageW(h,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(x,y));SendMessageW(h,WM_COMMAND,action,0);}
bool helperGone(){HANDLE mutex=OpenMutexW(SYNCHRONIZE,FALSE,gwm::vd::ipc::BrokerMutexName().c_str());if(mutex){CloseHandle(mutex);return false;}return true;}
uint32_t pixel(gwm::Graphics& graphics,gwm::Capture& capture,int x,int y){
    if(!capture.texture)return 0;
    const UINT sx=UINT(capture.clientOffset.x+x),sy=UINT(capture.clientOffset.y+y);
    D3D11_TEXTURE2D_DESC original{};capture.texture->GetDesc(&original);if(sx>=original.Width||sy>=original.Height)return 0;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> sample;winrt::check_hresult(graphics.device->CreateTexture2D(&desc,nullptr,sample.put()));
    const D3D11_BOX box{sx,sy,0,sx+1,sy+1,1};graphics.context->CopySubresourceRegion(sample.get(),0,0,0,0,capture.texture.get(),0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(graphics.context->Map(sample.get(),0,D3D11_MAP_READ,0,&mapped));const auto color=*static_cast<const uint32_t*>(mapped.pData)&0xffffffu;graphics.context->Unmap(sample.get(),0);return color;
}
bool liveCanvas(HWND window){
    HWND target=CreateWindowW(L"STATIC",L"GWM canvas GPU probe",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool live{};
    {
        gwm::Graphics graphics(target);gwm::Capture capture(window,graphics);std::set<uint32_t> colors;
        live=until([&]{capture.update(graphics);if(capture.state==GWM_LIVE)colors.insert(pixel(graphics,capture,120,220));return capture.frames>=6&&colors.contains(0xd21e28)&&colors.contains(0x1e32d2);},3000);
    }
    DestroyWindow(target);return live;
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    if(!gwm::test::waitForUnusedDisplay())return 77;
    const auto dpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(co))return 1;
    gwm::test::VirtualSource a(80,80),b(520,80),c(960,80);
    const auto first=a.window.load(),second=b.window.load(),third=c.window.load();
    RECT originalA{},originalB{},originalC{};GetWindowRect(first,&originalA);GetWindowRect(second,&originalB);GetWindowRect(third,&originalC);
    HMODULE dll=LoadLibraryW(argv[1]);int result{};
    try{
        require(dll&&first&&second&&third,"setup_three_disposable_source_windows");
        const auto start=reinterpret_cast<decltype(&WM_StartMapping)>(GetProcAddress(dll,"WM_StartMapping"));
        const std::array<uint32_t,4> windows{uint32_t(uintptr_t(second)),uint32_t(uintptr_t(first)),uint32_t(uintptr_t(third)),uint32_t(uintptr_t(second))};
        const auto begin=[&]{return start(windows.data(),4,2,2,60,380,900,540,0,0,380,250,nullptr,0);};
        require(begin()==GWM_OK,"original_14_parameter_start");
        require(begin()==GWM_ALREADY_STARTED,"duplicate_start_rejected");
        require(until([]{const auto display=gwm::vd::InspectOwnedDisplay();return canvas()&&display.active;}),"canvas_asynchronous_display_ready");
        gwm::vd::RecoveryClient observer;std::array<uint64_t,4> copied{};std::copy(windows.begin(),windows.end(),copied.begin());
        require(!observer.connect(dll,copied),"test_observer_uses_same_immutable_host_whitelist");
        require(until([&]{command(0,gwm::vd::ui::Hide);return hidden(second);}),"native_menu_hides_source_to_owned_display");
        require(until([&]{const auto reply=observer.query();return reply.hiddenCount==1&&reply.remaining==1;}),"duplicate_slots_share_one_hidden_record");
        require(liveCanvas(canvas()),"formal_canvas_wgc_frames_continue_while_source_hidden");
        require(until([&]{command(3,gwm::vd::ui::Show);return at(second,originalB);}),"show_from_duplicate_slot_restores_original_rectangle");
        require(until([&]{command(1,gwm::vd::ui::Hide);return hidden(first);}),"hide_second_slot");
        require(until([&]{RECT r{};GetClientRect(canvas(),&r);SendMessageW(canvas(),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(r.right*3/4,r.bottom/4));return at(first,originalA);}),"double_click_restores_hidden_source_before_activation");
        require(until([&]{command(0,gwm::vd::ui::Hide);return hidden(second);}),"hide_before_display_loss");
        require(bool(gwm::vd::DetachOwnedDisplay()),"simulate_only_owned_display_path_loss");
        require(until([&]{const auto button=GetDlgItem(canvas(),gwm::vd::ui::Retry);return at(second,originalB)&&button&&IsWindowVisible(button);}),"display_loss_restores_source_and_exposes_native_retry");
        SendMessageW(canvas(),WM_COMMAND,gwm::vd::ui::Retry,0);
        require(until([&]{return gwm::vd::InspectOwnedDisplay().active&&observer.query().state==gwm::vd::ipc::State::Ready;}),"native_retry_reinitializes_virtual_display");
        std::printf("after_retry_original_positions A=%d B=%d C=%d\n",at(first,originalA),at(second,originalB),at(third,originalC));
        for(int slot=0;slot<3;++slot){const HWND source=slot==0?second:slot==1?first:third;require(until([&]{command(slot,gwm::vd::ui::Hide);const auto reply=observer.query();return hidden(source)&&std::find(reply.hidden,reply.hidden+reply.hiddenCount,uint64_t(uintptr_t(source)))!=reply.hidden+reply.hiddenCount;}),"hide_three_independent_windows");}
        const bool three=until([&]{return observer.query().remaining==3;});
        if(!three){const auto reply=observer.query();std::printf("diagnostic remaining=%u hidden=%u state=%u error=%lu status=%lu\n",reply.remaining,reply.hiddenCount,unsigned(reply.state),reply.error,reply.statusError);for(unsigned i=0;i<reply.hiddenCount;++i)std::printf("hidden[%u]=%llu\n",i,reply.hidden[i]);for(auto source:{first,second,third}){RECT r{};GetWindowRect(source,&r);std::printf("HWND %lu rect=%ld,%ld,%ld,%ld\n",uint32_t(uintptr_t(source)),r.left,r.top,r.right,r.bottom);}}
        require(three,"three_unique_recovery_records");
        PostMessageW(canvas(),WM_CLOSE,0,0);
        require(until([]{return !canvas();}),"canvas_closes_after_recovery");
        for(const auto& pair:std::array<std::pair<HWND,RECT>,3>{{{first,originalA},{second,originalB},{third,originalC}}})if(!at(pair.first,pair.second)){RECT actual{};GetWindowRect(pair.first,&actual);std::printf("restore diagnostic HWND=%lu expected=%ld,%ld,%ld,%ld actual=%ld,%ld,%ld,%ld\n",uint32_t(uintptr_t(pair.first)),pair.second.left,pair.second.top,pair.second.right,pair.second.bottom,actual.left,actual.top,actual.right,actual.bottom);}
        require(at(first,originalA)&&at(second,originalB)&&at(third,originalC),"all_three_sources_restored_before_canvas_gone");
        require(until(helperGone)&&!gwm::vd::InspectOwnedDisplay().active,"last_session_detaches_display_and_helper_exits");
        require(until([&]{const int code=begin();return code!=GWM_ALREADY_STARTED&&code==GWM_OK;}),"same_loaded_dll_restarts_after_hidden_session");
        const bool hiddenAgain=until([&]{command(2,gwm::vd::ui::Hide);return hidden(third);});
        if(!hiddenAgain){const auto reply=observer.query();RECT r{};GetWindowRect(third,&r);std::printf("restart diagnostic valid=%d state=%u op=%lu display=%lu remaining=%u rect=%ld,%ld,%ld,%ld\n",IsWindow(third),unsigned(reply.state),reply.error,reply.statusError,reply.remaining,r.left,r.top,r.right,r.bottom);}
        require(hiddenAgain,"restarted_session_hides_third_source");
        PostMessageW(third,WM_CLOSE,0,0);require(until([&]{return !IsWindow(third);}),"closed_source_is_not_moved_again");
        PostMessageW(canvas(),WM_CLOSE,0,0);require(until([]{return !canvas();}),"close_with_invalid_hidden_source_finishes");
        require(until(helperGone),"second_session_helper_exits");
        require(until([&]{const int code=begin();return code!=GWM_ALREADY_STARTED&&code==GWM_OK;}),"start_third_session");
        PostMessageW(canvas(),WM_CLOSE,0,0);require(until([]{return !canvas();}),"close_during_async_initialization");
        require(until(helperGone)&&!gwm::vd::InspectOwnedDisplay().active,"initialization_close_leaves_no_active_path_or_helper");
    }catch(const std::exception& error){std::printf("FAIL %s\n",error.what());result=1;}
    if(canvas()){PostMessageW(canvas(),WM_CLOSE,0,0);until([]{return !canvas();});until(helperGone);}
    if(dll)FreeLibrary(dll);
    winrt::clear_factory_cache();CoUninitialize();if(dpi)SetThreadDpiAwarenessContext(dpi);
    std::printf("canvas_virtual: %d checks, result=%d\n",checks,result);return result;
}
