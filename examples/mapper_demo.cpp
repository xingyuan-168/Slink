#include "GameWindowMapper.h"
#include <windows.h>
#include <iostream>
#include <vector>
int wmain(int argc,wchar_t** argv){
    if(argc<2){std::cout<<"Developer host: mapper_demo HWND [HWND ...]\n";return 1;}
    std::vector<uint32_t> handles;
    for(int i=1;i<argc;++i)handles.push_back(wcstoul(argv[i],nullptr,0));
    int count=static_cast<int>(handles.size());
    int result=WM_StartMapping(handles.data(),count,1,count,100,100,1000,600,0,0,800,600,nullptr,0);
    std::cout<<"Start result="<<result<<"\nClose the canvas to release mapping.\n";
    if(result)return result;
    while(true){
        HWND h=FindWindowW(L"GameWindowMapper.Canvas.v1",nullptr);DWORD pid=0;
        if(!h||!GetWindowThreadProcessId(h,&pid)||pid!=GetCurrentProcessId())break;
        Sleep(50);
    }
    // Give the owner's final thread teardown a brief opportunity before this
    // demonstration host exits; a persistent E-language host needs no such wait.
    Sleep(100);
    return 0;
}
