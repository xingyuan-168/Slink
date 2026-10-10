#pragma once
#include "virtual/protocol.h"
#include "virtual/display_manager.h"
#include <cstdio>

namespace gwm::test {
inline bool waitForUnusedDisplay() {
    const auto deadline=GetTickCount64()+8000;
    for(;;){
        HANDLE broker=OpenMutexW(SYNCHRONIZE,FALSE,vd::ipc::BrokerMutexName().c_str());
        if(!broker)break;
        CloseHandle(broker);
        if(GetTickCount64()>=deadline){std::puts("SKIP: leave the existing mapping/recovery broker untouched");return false;}
        Sleep(100);
    }
    const auto display=vd::InspectOwnedDisplay();
    if(!display||!display.installed||display.active){std::puts("SKIP: requires an installed own driver and an inactive own display path");return false;}
    return true;
}
}
