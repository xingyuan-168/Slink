#pragma once
#include "model.h"
#include "virtual/controller.h"
#include <condition_variable>
#include <thread>

namespace gwm {
extern HINSTANCE module;
class Engine {
public:
    Engine(Config,int,int,int,int);
    ~Engine();
    int start();
    void wait();
    void stop();
    bool stopped() const {return stopRequested.load();}
    Config snapshot();
    HWND window() const {return hwnd.load();}
    int canvasState() const {return state.load();}
    uint32_t presented() const {return presentedFrames.load();}
    uint32_t elapsed() const {return static_cast<uint32_t>(nowMs()-startedAt);}
    uint64_t nextGeneration() {return ++generation;}
    void activate(int,int);
    static LRESULT CALLBACK wndProc(HWND,UINT,WPARAM,LPARAM) noexcept;
private:
    void uiMain() noexcept;
    void renderMain() noexcept;
    void requestStop() noexcept;
    Hit targetAt(int,int);
    void activateWindow(uint64_t);
    void contextMenu(int,int);
    void command(UINT);
    void updateStatusControls();
    void closeCanvas();
    vd::CanvasController controller;
    HWND retryButton=nullptr;
    Hit contextTarget{};
    std::atomic<bool> controllerStarted{false};
    std::atomic<bool> animateProgress{true};
    std::mutex configMutex;
    Config config;
    std::vector<Hit> hits;
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> presentedFrames{0};
    uint64_t startedAt=nowMs();
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> stopRequested{false};
    std::atomic<int> state{GWM_STARTING};
    std::thread uiThread,renderThread;
    std::mutex wakeMutex;
    std::condition_variable wake;
    bool uiReady=false,renderReady=false;
    int startupError=GWM_OK;
    int x,y,initialWidth,initialHeight;
};
}
