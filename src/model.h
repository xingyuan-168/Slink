#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "GameWindowMapper.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gwm {
inline constexpr int MaxSlots=64;
struct Region { int x=0,y=0,w=800,h=600; };
inline bool validRegion(Region r) {
    return r.x>=0 && r.y>=0 && r.w>0 && r.h>0 &&
        int64_t(r.x)+r.w<=INT32_MAX && int64_t(r.y)+r.h<=INT32_MAX;
}
inline bool validLayout(int r,int c) {return r>0 && c>0 && int64_t(r)*c<=MaxSlots;}
inline uint64_t nowMs() {return GetTickCount64();}
inline HWND toWindow(uint32_t h) {return reinterpret_cast<HWND>(static_cast<uintptr_t>(h));}
inline uint32_t fromWindow(HWND h) {return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));}
struct Image {UINT width=0,height=0;std::vector<uint8_t> pixels;};
struct Slot {
    uint32_t hwnd=0;uint64_t generation=0;Region roi;std::wstring title;
    std::shared_ptr<Image> background;bool force=false;
    int state=GWM_EMPTY,error=GWM_OK;uint64_t frameAt=0,frames=0;
};
struct Config {
    std::vector<Slot> slots;Region defaults;
    int rows=1,cols=1,fps=30;std::shared_ptr<Image> background;
};
struct Hit {RECT rect{};size_t slot=0;uint64_t generation=0;uint32_t hwnd=0;};
inline RECT tileRect(int width,int height,int rows,int cols,int i) {
    int row=i/cols,col=i%cols;
    return {int(int64_t(col)*width/cols),int(int64_t(row)*height/rows),
        int(int64_t(col+1)*width/cols),int(int64_t(row+1)*height/rows)};
}
std::shared_ptr<Image> decodeImage(const uint8_t*,int);
const wchar_t* stateText(int);
}
