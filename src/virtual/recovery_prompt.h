#pragma once
#include <windows.h>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gwm::vd {
// Only appears when an exited host still has windows awaiting recovery.
class RecoveryPrompt {
public:
    explicit RecoveryPrompt(std::function<void()> retry) : retry_(std::move(retry)) {}
    ~RecoveryPrompt();
    void update(unsigned remaining, DWORD error, const std::wstring& message);
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void run();
    void refresh(HWND);
    std::mutex mutex_;
    unsigned remaining_{};
    DWORD error_{};
    std::wstring message_;
    std::function<void()> retry_;
    std::thread thread_;
    HANDLE ready_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    DWORD threadId_{};
    HWND text_{},button_{};
    HFONT font_{};
};
}
