#include "virtual/recovery_prompt.h"
#include <atomic>
#include <cstdio>

namespace {
HWND prompt(){HWND h{};while((h=FindWindowExW(nullptr,h,L"GameWindowMapper.Recovery.v1",nullptr))){DWORD pid{};GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())return h;}return nullptr;}
template<class F> bool until(F predicate){const auto end=GetTickCount64()+3000;do{if(predicate())return true;Sleep(20);}while(GetTickCount64()<end);return predicate();}
}
int wmain(){
    std::atomic<unsigned> retries{};bool passed{};
    {
        gwm::vd::RecoveryPrompt view([&]{++retries;});
        view.update(2,ERROR_BUSY,L"测试恢复失败；未操作任何源窗口");
        passed=until([]{const auto h=prompt();return h&&IsWindowVisible(h)&&GetDlgItem(h,1);});
        if(passed){SendMessageW(prompt(),WM_COMMAND,MAKEWPARAM(1,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(prompt(),1)));passed=retries==1;}
        if(passed){SendMessageW(prompt(),WM_CLOSE,0,0);passed=until([]{return prompt()&&IsIconic(prompt());});}
        view.update(0,0,L"");passed=passed&&until([]{return prompt()&&!IsWindowVisible(prompt());});
    }
    passed=passed&&!prompt();std::printf("recovery_prompt: show, retry, minimize, hide, destroy = %s\n",passed?"PASS":"FAIL");return passed?0:1;
}
