#include "GameWindowMapper.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic_bool interrupted{false};
BOOL WINAPI control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        interrupted = true;
        return TRUE;
    }
    return FALSE;
}
int number(const std::wstring& text) {
    size_t end{};
    long long n = std::stoll(text, &end, 10);
    if (end != text.size() || n < INT32_MIN || n > INT32_MAX) throw std::runtime_error("Invalid integer");
    return static_cast<int>(n);
}
void handles(const std::wstring& text, std::vector<uint32_t>& output) {
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L',', start);
        auto item = text.substr(start, end == std::wstring::npos ? end : end - start);
        size_t consumed{};
        auto n = std::stoull(item, &consumed, item.starts_with(L"0x") || item.starts_with(L"0X") ? 16 : 10);
        if (item.empty() || item.front() == L'-' || consumed != item.size() || n > UINT32_MAX)
            throw std::runtime_error("HWND must be a 32-bit unsigned decimal or hexadecimal value");
        output.push_back(static_cast<uint32_t>(n));
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start == text.size()) throw std::runtime_error("Trailing comma in HWND list");
    }
}
void check(int result, const char* operation) {
    if (result == GWM_OK) return;
    std::cerr << operation << " failed, code=" << result << '\n';
    wchar_t message[1024]{};
    int32_t required{};
    WM_GetLastErrorTextW(message, 1024, &required);
    std::wcerr << message << L'\n';
    throw std::runtime_error("DLL call failed");
}
void usage() {
    std::cout << "mapper_demo --hwnds 0x123456,456789 [options]\n"
        "  --rows N --cols N                 default: 2 x 2\n"
        "  --region X Y W H                 source client pixels; default 0 0 800 600\n"
        "  --canvas X Y W H                 default 80 80 1100 720\n"
        "  --title SLOT TEXT                repeatable Unicode titles\n"
        "  --fps 15|30|60                   default 30\n"
        "  --duration SECONDS               0 = wait until canvas closes\n"
        "Prints status once per second. Ctrl+C stops and releases the DLL.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<uint32_t> windows;
    std::vector<std::pair<int, std::wstring>> titles;
    int rows=2, cols=2, x=80, y=80, width=1100, height=720;
    int sx=0, sy=0, sw=800, sh=600, fps=30, duration=0;
    bool started = false;
    try {
        for (int i=1; i<argc; ++i) {
            const std::wstring option = argv[i];
            auto next = [&]() -> std::wstring { if (++i >= argc) throw std::runtime_error("Missing argument"); return argv[i]; };
            if (option==L"--help" || option==L"-h") { usage(); return 0; }
            if (option==L"--hwnds") handles(next(), windows);
            else if (option==L"--rows") rows=number(next());
            else if (option==L"--cols") cols=number(next());
            else if (option==L"--fps") fps=number(next());
            else if (option==L"--duration") duration=number(next());
            else if (option==L"--region") { sx=number(next()); sy=number(next()); sw=number(next()); sh=number(next()); }
            else if (option==L"--canvas") { x=number(next()); y=number(next()); width=number(next()); height=number(next()); }
            else if (option==L"--title") { const int slot=number(next()); titles.emplace_back(slot, next()); }
            else throw std::runtime_error("Unknown option (see --help)");
        }
        if (windows.empty()) { usage(); return 2; }
        if (duration < 0 || windows.size() > INT32_MAX) throw std::runtime_error("Invalid duration or handle count");
        check(WM_StartMapping(windows.data(), static_cast<int32_t>(windows.size()), rows, cols, x, y, width, height, sx, sy, sw, sh), "WM_StartMapping");
        started = true;
        check(WM_SetFrameRate(fps), "WM_SetFrameRate");
        for (const auto& [slot, title] : titles) check(WM_SetTitleW(slot, title.data(), static_cast<int32_t>(title.size())), "WM_SetTitleW");
        SetConsoleCtrlHandler(control, TRUE);
        const ULONGLONG begin = GetTickCount64();
        ULONGLONG nextReport = begin;
        while (!interrupted) {
            int32_t state{}, count{}, currentRows{}, currentCols{};
            const int status = WM_GetCanvasStatus(&state, &count, &currentRows, &currentCols);
            if (status == GWM_NOT_STARTED || state == GWM_STOPPED) break;
            check(status, "WM_GetCanvasStatus");
            const ULONGLONG now = GetTickCount64();
            if (duration && now-begin >= static_cast<ULONGLONG>(duration)*1000) break;
            if (now >= nextReport) {
                uint32_t canvas{};
                WM_GetCanvasWindow(&canvas);
                std::cout << "canvas=" << canvas << " state=" << state << " slots=" << count << '\n';
                for (int slot=1; slot<=count; ++slot) {
                    uint32_t hwnd{}, age{}; int32_t slotState{}, error{};
                    check(WM_GetSlotStatus(slot, &hwnd, &slotState, &error, &age), "WM_GetSlotStatus");
                    std::cout << "slot=" << slot << " hwnd=" << hwnd << " state=" << slotState << " error=" << error << " frame_age_ms=" << age << '\n';
                }
                std::cout.flush();
                nextReport = now + 1000;
            }
            Sleep(50);
        }
        check(WM_Stop(), "WM_Stop");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (started) WM_Stop();
        return 1;
    }
}
