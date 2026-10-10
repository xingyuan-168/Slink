#include "virtual/window_parking.h"

#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace gwm::vd;
namespace {
int checks{};
void require(bool passed, const char* message) {
    ++checks;
    if (!passed) throw std::runtime_error(message);
}
void require(const ParkingResult& value, const char* message) {
    if (!value) std::wcerr << L"parking error=" << parkingCodeText(value.code) << L" win32=" << value.win32Error
        << L" retained=" << value.recordRetained << L'\n';
    require(static_cast<bool>(value), message);
}
RECT rectangle(HWND window) { RECT rect{}; require(GetWindowRect(window, &rect) != FALSE, "read rectangle"); return rect; }
bool equal(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
template<class Predicate> bool until(Predicate predicate, DWORD timeout = 2000) {
    const auto end = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(10); } while (GetTickCount64() < end);
    return predicate();
}

class Source {
public:
    HWND window{};
    std::atomic<bool> blockMove{};
    std::atomic<bool> hang{};
    Source(LONG left, LONG top) {
        std::promise<HWND> created;
        auto future = created.get_future();
        thread_ = std::thread([this, left, top, promise = std::move(created)]() mutable {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            auto handle = CreateWindowExW(0, L"GWM.Parking.Backend.Test", L"屏幕墙本轮窗口恢复测试",
                WS_OVERLAPPEDWINDOW, left, top, 300, 210, nullptr, nullptr, GetModuleHandleW(nullptr), this);
            if (handle) ShowWindow(handle, SW_SHOWNOACTIVATE);
            promise.set_value(handle);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        });
        window = future.get();
        if (!window) throw std::runtime_error("create test window");
    }
    ~Source() {
        hang = false; blockMove = false;
        if (window && IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    void destroy() {
        if (window && IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        require(until([&] { return !IsWindow(window); }), "test source closed");
        if (thread_.joinable()) thread_.join();
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto source = reinterpret_cast<Source*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            source = static_cast<Source*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(source));
        }
        if (source && message == WM_APP + 1) { while (source->hang.load()) Sleep(10); return 0; }
        if (source && source->blockMove && message == WM_WINDOWPOSCHANGING) {
            auto position = reinterpret_cast<WINDOWPOS*>(lparam);
            position->flags |= SWP_NOMOVE | SWP_NOSIZE;
        }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }
private:
    std::thread thread_;
};
class ClientProcess {
public:
    HANDLE process{};
    ClientProcess(const wchar_t* otherExecutable = nullptr, const wchar_t* mode = L"--session-only") {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        const wchar_t* executable = otherExecutable ? otherExecutable : path;
        std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &information) != FALSE, "launch dedicated client process");
        process = information.hProcess; CloseHandle(information.hThread);
    }
    ~ClientProcess() {
        if (process) { if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0); CloseHandle(process); }
    }
    void exit() {
        require(TerminateProcess(process, 0) != FALSE, "end dedicated client process");
        require(WaitForSingleObject(process, 2000) == WAIT_OBJECT_0, "client process exited");
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--session-only") { Sleep(60000); return 0; }
    std::cout.setf(std::ios::unitbuf);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{}; cls.lpfnWndProc = Source::procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"GWM.Parking.Backend.Test";
    RegisterClassW(&cls);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    POINT point{};
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor)) return 1;
    const RECT bounds = monitor.rcWork;
    if (argc > 1 && std::wstring(argv[1]) == L"--source-only") {
        Source remote(bounds.left + 50, bounds.top + 50); Sleep(60000); return 0;
    }
    std::unique_ptr<WindowParking> parking;
    try {
        Source first(bounds.left + 20, bounds.top + 20);
        Source second(bounds.left + 40, bounds.top + 60);
        const auto original = rectangle(first.window);
        const auto foreground = GetForegroundWindow();
        bool failPersistence = false, beforeFirstMove = false;
        int journalWrites = 0;
        std::vector<ParkingRecord> journal;
        ParkingOptions options;
        options.operationTimeoutMs = 750;
        options.persist = [&](std::span<const ParkingRecord> records) {
            ++journalWrites;
            if (failPersistence) return false;
            if (!records.empty() && records.front().window == reinterpret_cast<uintptr_t>(first.window) && journal.empty())
                beforeFirstMove = equal(rectangle(first.window), original);
            journal.assign(records.begin(), records.end()); return true;
        };
        parking = std::make_unique<WindowParking>(options);
        SessionIdentity session;
        require(parking->registerSession(GetCurrentProcess(), session), "register current host");
        require(parking->sessionAlive(session), "session handle alive");
        require(parking->hide({}, first.window, bounds).code == ParkingCode::invalidSession, "unknown session rejected");
        RECT tooSmall{bounds.left, bounds.top, bounds.left + 10, bounds.top + 10};
        require(parking->hide(session, first.window, tooSmall).code == ParkingCode::targetTooSmall, "too small rejected before move");
        RECT offscreen{100000, 100000, 101000, 101000};
        require(parking->hide(session, first.window, offscreen).code == ParkingCode::targetUnavailable, "off-desktop parking rejected");
        require(parking->hide(session, first.window, bounds), "hide normal window");
        require(beforeFirstMove && journalWrites > 0 && journal.size() == 1, "durable record written before first move");
        require(GetForegroundWindow() == foreground, "hide does not activate source");
        require(parking->isHidden(first.window) && parking->owns(session, first.window), "ownership query");
        const auto saved = parking->snapshot();
        require(parking->hide(session, first.window, bounds).code == ParkingCode::alreadyHidden, "duplicate hide idempotent");
        require(equal(parking->snapshot().front().originalRect, original), "duplicate does not overwrite original");
        require(!parking->unregisterSession(session), "cannot discard active recovery records");

        ClientProcess client;
        SessionIdentity secondSession;
        require(parking->registerSession(client.process, secondSession), "register independent host");
        require(parking->hide(secondSession, first.window, bounds).code == ParkingCode::ownedByAnotherSession, "other session cannot capture ownership");
        require(parking->show(secondSession, first.window).code == ParkingCode::ownedByAnotherSession, "other session cannot restore ownership");
        require(parking->hide(secondSession, second.window, bounds), "second host parks independent window");
        client.exit();
        const auto exited = parking->restoreExitedSessions();
        require(exited.complete() && exited.results.size() == 1 && parking->isHidden(first.window) && !parking->isHidden(second.window),
            "host exit restores only its own windows");
        require(parking->show(session, first.window), "restore original position");
        require(equal(rectangle(first.window), original) && parking->snapshot().empty() && journal.empty(), "normal restore verified and journal cleared");

        ShowWindowAsync(first.window, SW_SHOWMAXIMIZED);
        require(until([&] { return IsZoomed(first.window) != FALSE; }), "maximize test source");
        require(parking->hide(session, first.window, bounds), "maximized source can be parked");
        require(!IsZoomed(first.window) && !IsIconic(first.window), "parked source is renderable");
        require(parking->show(session, first.window), "maximized source restored");
        require(IsZoomed(first.window) != FALSE, "maximized state preserved");
        ShowWindowAsync(first.window, SW_SHOWMINNOACTIVE);
        require(until([&] { return IsIconic(first.window) != FALSE; }), "minimize previously maximized source");
        require(parking->hide(session, first.window, bounds), "minimized maximized source parked");
        require(parking->show(session, first.window), "minimized maximized source restored");
        ShowWindowAsync(first.window, SW_RESTORE);
        require(until([&] { return IsZoomed(first.window) != FALSE; }), "restore-to-maximized flag preserved");
        ShowWindowAsync(first.window, SW_SHOWNOACTIVATE);
        require(until([&] { return !IsZoomed(first.window); }), "normal source restored for next test");
        ShowWindowAsync(first.window, SW_SHOWMINNOACTIVE);
        require(until([&] { return IsIconic(first.window) != FALSE; }), "minimize test source");
        require(parking->hide(session, first.window, bounds), "minimized source can be parked");
        require(!IsIconic(first.window), "minimized source made renderable");
        require(parking->show(session, first.window), "minimized source restored");
        require(IsIconic(first.window) != FALSE, "minimized state preserved");
        ShowWindowAsync(first.window, SW_SHOWNOACTIVATE);
        require(until([&] { return !IsIconic(first.window); }), "source normalized");

        failPersistence = true;
        const auto before = rectangle(first.window);
        auto failed = parking->hide(session, first.window, bounds);
        require(failed.code == ParkingCode::persistenceFailed && !failed.recordRetained, "failed journal blocks initial movement");
        require(equal(rectangle(first.window), before) && parking->snapshot().empty(), "failed journal leaves original unchanged");
        failPersistence = false;
        require(parking->hide(session, first.window, bounds), "park before restore journal error");
        failPersistence = true;
        failed = parking->show(session, first.window);
        require(failed.code == ParkingCode::persistenceFailed && failed.recordRetained, "restore journal failure retains record");
        failPersistence = false;
        require(parking->show(session, first.window), "retry can clear restored record");

        require(parking->hide(session, first.window, bounds), "park before move refusal");
        first.blockMove = true;
        failed = parking->show(session, first.window);
        require(!failed && failed.recordRetained && parking->snapshot().size() == 1, "refused restore retains journal");
        first.blockMove = false;
        require(parking->show(session, first.window), "retry after move refusal");

        require(parking->hide(session, first.window, bounds), "park before unresponsive source");
        first.hang = true; PostMessageW(first.window, WM_APP + 1, 0, 0); Sleep(50);
        const auto started = GetTickCount64();
        failed = parking->show(session, first.window);
        require(!failed && failed.recordRetained && GetTickCount64() - started < 2000, "hung source has bounded restore and retains record");
        first.hang = false;
        require(parking->show(session, first.window), "retry after source resumes");

        require(parking->hide(session, first.window, bounds), "park before helper restart simulation");
        const auto recovery = parking->snapshot();
        parking.reset();
        parking = std::make_unique<WindowParking>(options);
        auto invalid = recovery; invalid.front().version = 999;
        require(parking->importRecords(invalid).code == ParkingCode::invalidRecoveryRecord && parking->snapshot().empty(), "invalid journal rejected atomically");
        require(parking->importRecords(recovery), "load recovery journal");
        require(parking->restoreAll().complete() && equal(rectangle(first.window), before), "journal restores without original host session");
        require(parking->registerSession(GetCurrentProcess(), session), "register after restart");
        require(parking->hide(session, first.window, bounds), "park before source close");
        first.destroy();
        const auto removed = parking->show(session, first.window);
        require(removed.code == ParkingCode::sourceGone && parking->snapshot().empty(), "destroyed window record removed without moving another HWND");

        require(parking->hide(session, second.window, bounds), "park before stale cookie");
        const auto secondBefore = rectangle(second.window);
        RemovePropW(second.window, L"GameWindowMapper.Parking.Owner.v1");
        const auto stale = parking->show(session, second.window);
        require(stale.code == ParkingCode::sourceGone && equal(rectangle(second.window), secondBefore), "missing lifetime cookie cannot move a potentially reused HWND");
        {
            Source detachedOrigin(bounds.left + 80, bounds.top + 80);
            require(parking->hide(session, detachedOrigin.window, bounds), "park fallback recovery fixture");
            auto unavailableMonitor = parking->snapshot();
            auto& record = unavailableMonitor.front();
            // Synthetic journal metadata exercises topology loss without
            // changing or disconnecting any of the user's actual monitors.
            wcscpy_s(record.originalMonitorIdentity, L"GWM.Test.DisconnectedPhysicalMonitor");
            wcscpy_s(record.virtualMonitorIdentity, L"GWM.Test.DisconnectedVirtualMonitor");
            OffsetRect(&record.originalRect, 100000, 100000);
            parking.reset();
            parking = std::make_unique<WindowParking>(options);
            require(parking->importRecords(unavailableMonitor), "import detached-monitor recovery fixture");
            const auto restored = parking->restoreAll();
            require(restored.complete(), "disconnected original falls back to physical primary");
            const auto visible = rectangle(detachedOrigin.window);
            require(visible.left >= bounds.left && visible.top >= bounds.top && visible.right <= bounds.right && visible.bottom <= bounds.bottom,
                "fallback rectangle fully visible");
            require(parking->registerSession(GetCurrentProcess(), session), "register after fallback recovery");
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--x86-source") {
            ClientProcess remote(argv[2], L"--source-only");
            const auto processId = GetProcessId(remote.process);
            HWND otherWindow{};
            require(until([&] {
                HWND current{};
                while ((current = FindWindowExW(nullptr, current, cls.lpszClassName, nullptr))) {
                    DWORD owner{}; GetWindowThreadProcessId(current, &owner);
                    if (owner == processId) { otherWindow = current; return true; }
                }
                return false;
            }), "find owned x86 source window");
            BOOL wow64{};
            require(IsWow64Process(remote.process, &wow64) && wow64, "source process is x86");
            const auto initial = rectangle(otherWindow);
            require(parking->hide(session, otherWindow, bounds), "x64 helper parks x86 HWND");
            require(parking->show(session, otherWindow), "x64 helper restores x86 HWND");
            require(equal(rectangle(otherWindow), initial), "cross-bitness restores exact rectangle");
        }
        require(parking->unregisterSession(session), "unregister after cleanup");
        require(parking->restoreAll().complete() && journal.empty(), "all test sessions finished");
        std::cout << "PASS " << checks << " parking checks; only owned test windows used, no virtual driver installed.\n";
    } catch (const std::exception& error) {
        // Source destructors already closed all owned windows. Callback captures
        // are out of scope here, so do not invoke the persistence hook again.
        parking.reset();
        std::cerr << "FAIL: " << error.what() << '\n';
        UnregisterClassW(cls.lpszClassName, cls.hInstance);
        return 1;
    }
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    return 0;
}
