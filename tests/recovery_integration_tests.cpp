// Opt-in, short, real x86 DLL payload -> x64 recovery broker test. Moves only
// disposable windows created here; requires an installed, detached own driver.
#include "virtual/client.h"
#include "virtual/display_manager.h"
#include "virtual_test_source.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
int failures{};
void check(bool pass, const char* name) {
    std::printf("{\"test\":\"%s\",\"pass\":%s}\n", name, pass ? "true" : "false");
    std::fflush(stdout);
    if (!pass) ++failures;
}
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Module {
    HMODULE value{};
    explicit Module(const wchar_t* path) : value(LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {}
    ~Module() { if (value) FreeLibrary(value); }
};
bool waitFor(const std::function<bool()>& predicate, DWORD timeout = 6000) {
    const auto deadline = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(50); } while (GetTickCount64() < deadline);
    return predicate();
}
bool at(HWND window, const RECT& expected) {
    RECT actual{}; return GetWindowRect(window, &actual) && EqualRect(&actual, &expected);
}
uint64_t raw(HWND window) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window)); }
bool contains(const gwm::vd::ipc::Reply& reply, HWND window) {
    return std::find(reply.hidden, reply.hidden + reply.hiddenCount, raw(window)) != reply.hidden + reply.hiddenCount;
}
bool ready(gwm::vd::RecoveryClient& client) {
    auto reply = client.initialize();
    if (reply.error && reply.error != ERROR_IO_PENDING) return false;
    return waitFor([&] {
        reply = client.query();
        return !reply.error && !reply.statusError && reply.active && reply.state == gwm::vd::ipc::State::Ready;
    }, 10000);
}
bool hide(gwm::vd::RecoveryClient& client, HWND window) {
    gwm::vd::ipc::Reply reply;
    const bool finished = waitFor([&] { reply = client.hide(raw(window)); return reply.error != ERROR_IO_PENDING; });
    if (!finished || reply.error) std::printf("{\"hide_error\":%lu,\"status_error\":%lu}\n", reply.error, reply.statusError);
    return finished && !reply.error && contains(reply, window);
}
bool release(gwm::vd::RecoveryClient& client) {
    if (!client.registered()) return true;
    gwm::vd::ipc::Reply reply;
    const bool finished = waitFor([&] { reply = client.release(); return !reply.error && !reply.remaining; }, 10000);
    if (!finished) std::printf("{\"release_error\":%lu,\"remaining\":%lu}\n", reply.error, reply.remaining);
    return finished && !client.registered();
}
int crashHost(const wchar_t* dll, const wchar_t* handleText) {
    Module module(dll);
    if (!module.value) return 2;
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(handleText, nullptr, 10)));
    const std::array<uint64_t, 1> windows{raw(window)};
    gwm::vd::RecoveryClient client;
    if (client.connect(module.value, windows) || !ready(client) || !hide(client, window)) return 3;
    check(contains(client.query(), window), "child_host_window_hidden_before_exit");
    // Deliberately bypass all C++ destructors and Release. The source window
    // lives in the parent, so its position proves independent crash recovery.
    ExitProcess(failures ? 4 : 0);
}
int manualParking(const wchar_t* dll, const wchar_t* handleText) {
    wchar_t* end{};
    const uint64_t value = _wcstoui64(handleText, &end, 10);
    if (!end || *end || !value || value > UINTPTR_MAX || !IsWindow(reinterpret_cast<HWND>(uintptr_t(value)))) {
        std::puts("ERROR: pass the current HWND in decimal, not a process ID."); return 2;
    }
    if (!gwm::vd::InspectOwnedDisplay().installed) {
        std::puts("ERROR: install the owned driver before this manual acceptance probe."); return 2;
    }
    const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Module module(dll);
    gwm::vd::RecoveryClient client;
    const std::array<uint64_t, 1> windows{value};
    bool parked = module.value && !client.connect(module.value, windows) && ready(client) &&
        hide(client, reinterpret_cast<HWND>(uintptr_t(value)));
    if (parked) {
        std::printf("HWND %llu is on the owned virtual display. Test background binding now.\n"
            "Press Enter here to restore its original position and release this session.\n",
            static_cast<unsigned long long>(value));
        std::fflush(stdout);
        std::getchar();
    } else {
        const auto reply = client.lastReply();
        std::printf("Parking failed: operation=%lu display=%lu state=%u\n", reply.error, reply.statusError, uint32_t(reply.state));
    }
    const bool restored = release(client);
    std::puts(restored ? "Restored; this host lease was released." : "Restore pending; the independent recovery helper retains the record and retries.");
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    return parked && restored ? 0 : 1;
}
HANDLE brokerProcess() {
    const auto name = gwm::vd::ipc::BrokerPipeName();
    Handle pipe{CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    ULONG pid{};
    if (pipe.value == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe.value, &pid)) return nullptr;
    return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && wcscmp(argv[1], L"--crash-host") == 0) return crashHost(argv[2], argv[3]);
    if (argc == 4 && wcscmp(argv[1], L"--park") == 0) return manualParking(argv[2], argv[3]);
    if (argc != 2) { std::puts("usage: recovery_integration_tests <DLL with embedded helper>"); return 2; }
    // Refuse to interfere with any already running mapping or active own path.
    if (WaitNamedPipeW(gwm::vd::ipc::BrokerPipeName().c_str(), 0) || GetLastError() != ERROR_FILE_NOT_FOUND) {
        std::puts("SKIP: an existing recovery broker must be left untouched"); return 77;
    }
    const auto before = gwm::vd::InspectOwnedDisplay();
    if (!before || !before.installed || before.active) {
        std::puts("SKIP: requires the installed own driver with no active own path"); return 77;
    }
    const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Module module(argv[1]);
    gwm::vd::RecoveryClient client;
    gwm::test::VirtualSource first(80, 80), second(520, 80);
    const auto a = first.window.load(), b = second.window.load();
    RECT originalA{}, originalB{};
    GetWindowRect(a, &originalA); GetWindowRect(b, &originalB);
    Handle broker;
    try {
        if (!a || !b || !module.value) throw std::runtime_error("test setup failed");
        const std::array<uint64_t, 2> duplicated{raw(a), raw(a)};
        const DWORD connected = client.connect(module.value, duplicated);
        std::printf("{\"connect_error\":%lu}\n", connected);
        if (connected) {
            const auto detail = client.lastReply();
            char message[2048]{};
            WideCharToMultiByte(CP_UTF8, 0, detail.message, -1, message, int(std::size(message)), nullptr, nullptr);
            std::printf("connection diagnostic: error=%lu status=%lu %s\n", detail.error, detail.statusError, message);
        }
        check(!connected, "embedded_payload_starts_native_x64_broker");
        if (connected) throw std::runtime_error("broker connection failed");
        broker.value = brokerProcess();
        check(ready(client), "asynchronous_display_initialization_ready");
        check(hide(client, a), "parent_host_hides_own_source");
        const auto duplicateState = client.query();
        check(duplicateState.hiddenCount == 1 && duplicateState.remaining == 1,
            "duplicate_hwnd_has_one_shared_recovery_record");
        check(client.show(raw(b)).error == ERROR_ACCESS_DENIED, "immutable_host_whitelist_enforced");
        check(hide(client, a), "idempotent_hide_preserves_original_placement");

        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --crash-host \"" + argv[1] + L"\" " + std::to_wstring(raw(b));
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) throw std::runtime_error("child host creation failed");
        Handle child{process.hProcess}, childThread{process.hThread};
        const bool exited = WaitForSingleObject(child.value, 30000) == WAIT_OBJECT_0;
        if (!exited) { TerminateProcess(child.value, 9); WaitForSingleObject(child.value, 5000); }
        DWORD exitCode{}; GetExitCodeProcess(child.value, &exitCode);
        check(exited && exitCode == 0, "second_host_hides_then_exits_without_release");
        check(waitFor([&] { return at(b, originalB); }), "exited_host_source_restored_automatically");
        auto state = client.query();
        check(contains(state, a) && !at(a, originalA) && state.active,
            "surviving_host_keeps_its_window_hidden_and_display_active");

        // The only two test sources are ours, and the child lease has exited.
        // Simulate removal of our path, never another vendor's display.
        check(bool(gwm::vd::DetachOwnedDisplay()), "simulate_owned_virtual_path_loss");
        check(waitFor([&] { return at(a, originalA) && client.query().remaining == 0; }),
            "virtual_display_loss_restores_remaining_source");
        state = client.query();
        check(state.state == gwm::vd::ipc::State::Unavailable && !state.active,
            "display_loss_reported_separately_from_capture_and_release");
    } catch (const std::exception& error) {
        std::printf("{\"exception\":\"%s\",\"pass\":false}\n", error.what()); ++failures;
    } catch (...) { ++failures; }
    check(release(client), "normal_release_clears_host_lease");
    check(at(a, originalA) && at(b, originalB), "both_original_window_rectangles_restored");
    check(broker.value && WaitForSingleObject(broker.value, 7000) == WAIT_OBJECT_0,
        "last_session_helper_exits_without_resident_process");
    check(!gwm::vd::InspectOwnedDisplay().active, "last_session_own_display_path_inactive");
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    std::printf("{\"summary\":true,\"failures\":%d,\"pass\":%s}\n", failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}
