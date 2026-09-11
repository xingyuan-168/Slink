// Integration harness for the public x86 DLL ABI. JSON Lines go to stdout.
// No DLL import linkage: every cycle explicitly stops and unloads the module.
#include <windows.h>
#include <psapi.h>
#include "GameWindowMapper.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* name) {
    std::printf("{\"test\":\"%s\",\"pass\":%s}\n", name, value ? "true" : "false");
    std::fflush(stdout);
    if (!value) ++failures;
}
void require(bool value, const char* name) {
    check(value, name);
    if (!value) throw std::runtime_error(name);
}
uint32_t bits(HWND window) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window)); }
HWND window(uint32_t value) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(value)); }
#define GWM_FUNCTIONS(X) \
    X(WM_StartMapping) X(WM_ReplaceWindows) X(WM_SetWindow) X(WM_RemoveWindow) \
    X(WM_AddWindow) X(WM_SetRegion) X(WM_SetLayout) X(WM_SetTitleA) X(WM_SetTitleW) \
    X(WM_SetBackground) X(WM_SetForceBackground) X(WM_SetFrameRate) \
    X(WM_GetCanvasWindow) X(WM_GetCanvasStatus) X(WM_GetSlotStatus) \
    X(WM_GetLastErrorTextW) X(WM_GetFrameCounters) X(WM_Stop)
struct Api {
    HMODULE module = nullptr;
#define FIELD(name) decltype(&::name) name = nullptr;
    GWM_FUNCTIONS(FIELD)
#undef FIELD
    explicit Api(const std::filesystem::path& path) {
        module = LoadLibraryW(path.c_str());
        if (!module) throw std::runtime_error("LoadLibrary failed; require sibling x86 GameWindowMapper.dll");
        try {
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(module, #name)); if (!name) throw std::runtime_error("Missing undecorated export " #name);
            GWM_FUNCTIONS(RESOLVE)
#undef RESOLVE
        } catch (...) { FreeLibrary(module); module = nullptr; throw; }
    }
    ~Api() { if (module) { WM_Stop(); FreeLibrary(module); } }
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
};
struct Child {
    PROCESS_INFORMATION process{};
    HWND hwnd = nullptr;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : process(other.process), hwnd(other.hwnd) { other.process = {}; other.hwnd = nullptr; }
    Child& operator=(Child&& other) noexcept {
        if (this != &other) { close(); process = other.process; hwnd = other.hwnd; other.process = {}; other.hwnd = nullptr; }
        return *this;
    }
    ~Child() { close(); }
    void close() {
        if (IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
                // Only a process created and retained by this harness can reach this path.
                TerminateProcess(process.hProcess, 125);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            process = {};
        }
        hwnd = nullptr;
    }
};
Child launch(const std::filesystem::path& path, int index, bool d3d) {
    Child child;
    std::wstring title = L"GWM_TEST_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(index);
    std::wstring command = L"\"" + path.wstring() + L"\" " + (d3d ? L"--d3d11" : L"--gdi") +
        L" --title \"" + title + L"\" --x " + std::to_wstring(20 + (index % 4) * 25) +
        L" --y " + std::to_wstring(20 + (index % 4) * 25);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, path.parent_path().c_str(), &startup, &child.process))
        throw std::runtime_error("Unable to launch dynamic_source.exe");
    const auto until = GetTickCount64() + 10000;
    while (GetTickCount64() < until) {
        child.hwnd = FindWindowW(L"GWM.DynamicSource", title.c_str());
        if (child.hwnd) break;
        if (WaitForSingleObject(child.process.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(30);
    }
    if (!child.hwnd) throw std::runtime_error("Dynamic source did not create independent HWND");
    return child;
}
struct Slot { uint32_t hwnd = 0, age = 0; int32_t state = 0, error = 0; };
Slot status(Api& api, int slot) {
    Slot s;
    const auto result = api.WM_GetSlotStatus(slot, &s.hwnd, &s.state, &s.error, &s.age);
    if (result != GWM_OK) throw std::runtime_error("GetSlotStatus failed");
    return s;
}
bool awaitState(Api& api, int slot, int desired, DWORD timeout = 10000) {
    const auto until = GetTickCount64() + timeout;
    do { if (status(api, slot).state == desired) return true; Sleep(30); } while (GetTickCount64() < until);
    return false;
}
int start(Api& api, const std::vector<uint32_t>& handles, int rows, int cols) {
    return api.WM_StartMapping(handles.data(), static_cast<int32_t>(handles.size()), rows, cols,
        760, 40, 800, 600, 0, 0, 640, 480);
}
struct Metrics { uint64_t cpu100ns = 0, privateBytes = 0; DWORD handles = 0; };
Metrics metrics() {
    Metrics m;
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    auto ticks = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    m.cpu100ns = ticks(kernel) + ticks(user);
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) m.privateBytes = memory.PrivateUsage;
    GetProcessHandleCount(GetCurrentProcess(), &m.handles);
    return m;
}
void performance(Api& api, int count, int seconds, int fps) {
    require(api.WM_SetFrameRate(fps) == GWM_OK, "set_requested_frame_rate");
    const auto before = metrics();
    const auto begin = GetTickCount64();
    const auto end = begin + static_cast<uint64_t>(seconds) * 1000;
    std::vector<uint64_t> live(count), fresh(count), samples(count);
    std::vector<uint32_t> capturedBefore(count);
    uint32_t presentedBefore=0, counterElapsed=0;
    for(int i=0;i<count;++i) require(api.WM_GetFrameCounters(i+1,&capturedBefore[i],&presentedBefore,&counterElapsed)==GWM_OK,"frame_counter_query");
    auto nextReport = begin + 5000;
    while (GetTickCount64() < end) {
        for (int i = 0; i < count; ++i) {
            auto s = status(api, i + 1); ++samples[i];
            if (s.state == GWM_LIVE) { ++live[i]; if (s.age <= 250) ++fresh[i]; }
        }
        Sleep(100);
        const auto now = GetTickCount64();
        if (now >= nextReport) {
            const auto m = metrics();
            std::printf("{\"progress_seconds\":%.3f,\"private_bytes\":%llu,\"handles\":%lu}\n",
                (now - begin) / 1000.0, static_cast<unsigned long long>(m.privateBytes), static_cast<unsigned long>(m.handles));
            std::fflush(stdout); nextReport = now + 5000;
        }
    }
    const auto after = metrics(); const auto elapsed = GetTickCount64() - begin;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const double cpu = elapsed ? (after.cpu100ns - before.cpu100ns) / (elapsed * 10000.0) * 100.0 / info.dwNumberOfProcessors : 0;
    uint32_t captured=0,presented=0;
    require(api.WM_GetFrameCounters(0,&captured,&presented,&counterElapsed)==GWM_OK,"canvas_counter_query");
    std::printf("{\"performance\":true,\"windows\":%d,\"requested_fps\":%d,\"elapsed_ms\":%llu,\"host_cpu_percent_total_machine\":%.3f,\"private_bytes_start\":%llu,\"private_bytes_end\":%llu,\"handles_start\":%lu,\"handles_end\":%lu,\"gpu_usage\":null,\"vram_bytes\":null,\"presentation_submit_fps\":%.3f,\"note\":\"WGC deliveries and successful Present submissions; source process CPU is excluded\"}\n",
        count, fps, static_cast<unsigned long long>(elapsed), cpu,
        static_cast<unsigned long long>(before.privateBytes), static_cast<unsigned long long>(after.privateBytes),
        static_cast<unsigned long>(before.handles), static_cast<unsigned long>(after.handles), uint32_t(presented-presentedBefore)*1000.0/elapsed);
    for (int i = 0; i < count; ++i) {
        require(api.WM_GetFrameCounters(i+1,&captured,&presented,&counterElapsed)==GWM_OK,"slot_counter_query");
        std::printf("{\"slot\":%d,\"samples\":%llu,\"live_samples\":%llu,\"fresh_age_le_250ms_samples\":%llu,\"wgc_delivery_fps\":%.3f}\n",
            i + 1, static_cast<unsigned long long>(samples[i]), static_cast<unsigned long long>(live[i]), static_cast<unsigned long long>(fresh[i]),uint32_t(captured-capturedBefore[i])*1000.0/elapsed);
        check(fresh[i] > 0, "slot_received_fresh_frame_during_measurement");
    }
}
void functional(Api& api, std::vector<Child>& children, const std::filesystem::path& sourcePath) {
    const uint32_t a = bits(children[0].hwnd), b = bits(children[1].hwnd);
    require(api.WM_Stop() == GWM_OK, "stop_before_start_is_safe");
    check(api.WM_StartMapping(&a, 1, 1, 1, 0, 0, 640, 480, 0, 0, 0, 100) == GWM_INVALID_ARGUMENT, "reject_zero_region_width");
    check(api.WM_StartMapping(nullptr, 1, 1, 1, 0, 0, 640, 480, 0, 0, 100, 100) == GWM_INVALID_ARGUMENT, "reject_missing_array");
    std::vector<uint32_t> order{b, 0, a, b};
    require(start(api, order, 2, 2) == GWM_OK, "start_scrambled_duplicates_empty");
    check(start(api, order, 2, 2) == GWM_ALREADY_STARTED, "reject_duplicate_start");
    for (int i = 0; i < 4; ++i) check(status(api, i + 1).hwnd == order[i], "fixed_input_order");
    check(status(api, 2).state == GWM_EMPTY, "empty_slot_preserved");
    check(awaitState(api, 1, GWM_LIVE), "source_reaches_live");
    check(awaitState(api, 4, GWM_LIVE), "duplicate_source_reaches_live");
    int32_t state = 0, count = 0, rows = 0, cols = 0;
    check(api.WM_GetCanvasStatus(&state, &count, &rows, &cols) == GWM_OK && state == GWM_RUNNING && count == 4 && rows == 2 && cols == 2, "canvas_status_matches_layout");
    using Inject = int32_t(GWM_CALL*)();
    if(auto inject=reinterpret_cast<Inject>(GetProcAddress(api.module,"_WM_TestInjectDeviceLoss@0"))){
        uint32_t canvas=0,before=0,after=0,presented=0,elapsed=0;
        require(api.WM_GetCanvasWindow(&canvas)==GWM_OK,"recovery_canvas_handle");
        require(api.WM_GetFrameCounters(1,&before,&presented,&elapsed)==GWM_OK,"recovery_counter_before");
        ShowWindow(window(canvas),SW_MINIMIZE);
        require(inject()==GWM_OK,"inject_device_removed_while_canvas_minimized");
        require(awaitState(api,1,GWM_RECOVERING),"device_loss_publishes_recovering");
        require(awaitState(api,1,GWM_LIVE),"device_recovers_while_canvas_minimized");
        require(api.WM_GetFrameCounters(1,&after,&presented,&elapsed)==GWM_OK&&after>=before,"device_recovery_counter_monotonic");
        check(status(api,1).error==GWM_OK,"device_recovery_clears_error");
        ShowWindow(window(canvas),SW_RESTORE);
    }
    check(api.WM_SetLayout(1, 3) == GWM_CAPACITY_EXCEEDED, "layout_cannot_truncate_slots");
    check(api.WM_SetRegion(0, 0, 0, 10, 10) == GWM_SLOT_OUT_OF_RANGE, "slot_numbers_start_at_one");
    check(api.WM_SetRegion(1, 0, 0, -1, 10) == GWM_INVALID_ARGUMENT, "reject_negative_region_width");
    check(api.WM_SetRegion(1, -50, -50, 800, 600) == GWM_INVALID_ARGUMENT, "reject_negative_region_origin");
    check(api.WM_SetRegion(1, 500, 400, 800, 600) == GWM_OK, "partially_outside_region_accepted");
    check(api.WM_SetRegion(4, 50, 50, 300, 200) == GWM_OK, "duplicate_has_independent_region");
    const wchar_t chinese[] = L"中文标题：测试";
    check(api.WM_SetTitleW(1, chinese, static_cast<int32_t>(wcslen(chinese))) == GWM_OK, "utf16_title");
    const char cp936[] = "\xd6\xd0\xce\xc4\xb1\xea\xcc\xe2";
    check(api.WM_SetTitleA(3, cp936, static_cast<int32_t>(sizeof(cp936) - 1)) == GWM_OK, "cp936_title");
    const uint8_t junk[] = {0xde, 0xad, 0xbe, 0xef};
    check(api.WM_SetBackground(0, junk, sizeof(junk)) == GWM_IMAGE_DECODE_FAILED, "reject_malformed_background");
    // Tiny, uncompressed 1x1 opaque BMP exercises byte-buffer decoding.
    const uint8_t bmp[] = {0x42,0x4d,58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,1,0,24,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x80,0xd0,0x20,0};
    check(api.WM_SetBackground(0, bmp, sizeof(bmp)) == GWM_OK, "decode_default_bmp");
    check(api.WM_SetBackground(1, bmp, sizeof(bmp)) == GWM_OK, "decode_slot_bmp");
    check(api.WM_SetForceBackground(1, 1) == GWM_OK && awaitState(api, 1, GWM_FORCED_BACKGROUND), "force_background_state");
    check(api.WM_SetForceBackground(1, 0) == GWM_OK && awaitState(api, 1, GWM_LIVE), "unforce_background_resumes_capture");
    check(api.WM_SetBackground(1, nullptr, 0) == GWM_OK, "clear_slot_background");
    ShowWindow(children[1].hwnd, SW_MINIMIZE);
    check(awaitState(api, 1, GWM_MINIMIZED), "minimize_state");
    ShowWindow(children[1].hwnd, SW_RESTORE);
    check(awaitState(api, 1, GWM_LIVE), "restore_resumes_capture");
    SetWindowPos(children[1].hwnd, nullptr, 60, 60, 700, 510, SWP_NOZORDER | SWP_NOACTIVATE);
    check(awaitState(api, 1, GWM_LIVE), "source_resize_keeps_capture");
    check(api.WM_RemoveWindow(1) == GWM_OK, "remove_only_clears_slot");
    check(status(api, 1).hwnd == 0 && status(api, 3).hwnd == a && status(api, 4).hwnd == b, "remove_does_not_shift");
    int32_t appended = 0;
    check(api.WM_AddWindow(a, &appended) == GWM_CAPACITY_EXCEEDED, "append_does_not_reuse_empty_slot");
    check(api.WM_SetLayout(2, 3) == GWM_OK && api.WM_AddWindow(a, &appended) == GWM_OK && appended == 5, "append_after_array_tail");
    std::vector<uint32_t> replacement{a, b, 0, a, b};
    check(api.WM_ReplaceWindows(replacement.data(), 5) == GWM_OK, "replace_array");
    for (int i = 0; i < 5; ++i) check(status(api, i + 1).hwnd == replacement[i], "replacement_order");
    // Close only the test process owned here; the old handle stays in its slot.
    children[1].close();
    check(awaitState(api, 2, GWM_INVALID), "closed_source_is_invalid");
    check(status(api, 1).hwnd == a && status(api, 3).hwnd == 0 && status(api, 4).hwnd == a, "closed_source_does_not_shift");
    children[1] = launch(sourcePath, 1000, true);
    check(api.WM_SetWindow(2, bits(children[1].hwnd)) == GWM_OK && awaitState(api, 2, GWM_LIVE), "replace_closed_handle_recovers");
    uint32_t canvas = 0;
    require(api.WM_GetCanvasWindow(&canvas) == GWM_OK && IsWindow(window(canvas)), "canvas_window_handle");
    RECT rect{}; GetClientRect(window(canvas), &rect);
    // Input is sent only to the DLL canvas created by this test process.
    SetForegroundWindow(window(canvas));
    SendMessageW(window(canvas), WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(rect.right / 6, rect.bottom / 4));
    Sleep(100);
    const auto activated = GetForegroundWindow() == children[0].hwnd;
    const auto activationStatus = status(api, 1);
    check(activated || activationStatus.error == GWM_ACTIVATION_DENIED, "double_click_activates_or_reports_denied");
    check(api.WM_SetFrameRate(17) == GWM_INVALID_ARGUMENT, "reject_unsupported_frame_rate");
    for (const int rate : {15, 30, 60}) check(api.WM_SetFrameRate(rate) == GWM_OK, "supported_frame_rate");
    std::atomic<bool> done{false}; std::atomic<int> unexpected{0};
    auto updater = [&] {
        while (!done.load()) {
            const auto result = api.WM_SetRegion(1, 0, 0, 640, 480);
            if (result != GWM_OK && result != GWM_NOT_STARTED && result != GWM_STOPPING) ++unexpected;
            std::this_thread::yield();
        }
    };
    std::thread first(updater), second(updater);
    Sleep(100);
    const auto stopped = api.WM_Stop();
    done = true; first.join(); second.join();
    check(stopped == GWM_OK && unexpected == 0, "concurrent_update_and_stop");
    check(api.WM_Stop() == GWM_OK, "repeated_stop_safe");
    check(!IsWindow(window(canvas)), "stop_destroys_canvas");
    check(IsWindow(children[0].hwnd), "stop_preserves_source");
}
}
int wmain(int argc, wchar_t** argv) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = std::filesystem::path(executable).parent_path();
    auto dll = directory / L"GameWindowMapper.dll";
    auto source = directory / L"dynamic_source.exe";
    int count = 4, cycles = 100, seconds = 5, fps = 30;
    bool allD3d = false, allGdi = false, performanceOnly=false, requireHook=false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--smoke") { cycles = 3; seconds = 2; }
        else if(arg==L"--performance-only") performanceOnly=true;
        else if(arg==L"--require-device-hook") requireHook=true;
        else if (arg == L"--windows" && i + 1 < argc) count = _wtoi(argv[++i]);
        else if (arg == L"--cycles" && i + 1 < argc) cycles = _wtoi(argv[++i]);
        else if (arg == L"--stress-seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (arg == L"--fps" && i + 1 < argc) fps = _wtoi(argv[++i]);
        else if (arg == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (arg == L"--source" && i + 1 < argc) source = argv[++i];
        else if (arg == L"--d3d11") allD3d = true;
        else if (arg == L"--gdi") allGdi = true;
        else { std::fprintf(stderr, "Usage: native_tests [--smoke] [--windows 1..64] [--cycles N] [--stress-seconds N] [--fps 15|30|60] [--dll path] [--source path] [--gdi|--d3d11]\n"); return 2; }
    }
    if (sizeof(void*) != 4 || count < 1 || count > 64 || cycles < 0 || seconds < 1 || (fps != 15 && fps != 30 && fps != 60) || (allGdi && allD3d)) return 2;
    try {
        if(performanceOnly)cycles=0;
        const int sourceCount=std::max(performanceOnly?1:2,count);
        std::vector<Child> children; children.reserve(sourceCount);
        for (int i = 0; i < sourceCount; ++i) children.push_back(launch(source, i, allD3d || (!allGdi && i % 2 != 0)));
        if(!performanceOnly){ Api api(dll);
            if(requireHook)require(GetProcAddress(api.module,"_WM_TestInjectDeviceLoss@0")!=nullptr,"required_device_test_hook_export");
            functional(api, children, source);
        }
        const auto initial = metrics();
        for (int cycle = 0; cycle < cycles; ++cycle) {
            Api api(dll);
            std::vector<uint32_t> one{bits(children[0].hwnd)};
            require(start(api, one, 1, 1) == GWM_OK, "cycle_start");
            if (cycle == 0 || cycle == cycles - 1) check(awaitState(api, 1, GWM_LIVE), "cycle_live");
            uint32_t canvas = 0; api.WM_GetCanvasWindow(&canvas);
            require(api.WM_Stop() == GWM_OK && !IsWindow(window(canvas)), "cycle_stop_before_unload");
        }
        const auto final = metrics();
        std::printf("{\"completed_unload_cycles\":%d,\"handle_delta\":%lld,\"private_bytes_delta\":%lld}\n", cycles,
            static_cast<long long>(final.handles) - initial.handles,
            static_cast<long long>(final.privateBytes) - static_cast<long long>(initial.privateBytes));
        std::vector<uint32_t> handles;
        for (int i = 0; i < count; ++i) handles.push_back(bits(children[i].hwnd));
        { Api api(dll);
          const int cols = std::min(count, 4), rows = (count + cols - 1) / cols;
          require(start(api, handles, rows, cols) == GWM_OK, "start_performance_run");
          for (int i = 1; i <= count; ++i) check(awaitState(api, i, GWM_LIVE), "performance_warmup_live");
          performance(api, count, seconds, fps);
        }
    } catch (const std::exception& error) { std::fprintf(stderr, "Harness error: %s\n", error.what()); ++failures; }
    std::printf("{\"summary\":true,\"failures\":%d,\"pass\":%s}\n", failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}
