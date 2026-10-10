#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Private x86 DLL <-> x64 recovery helper protocol. This is not a DLL export.
// A broker is shared by hosts in the same interactive logon, never by PID alone.
namespace gwm::vd::ipc {
inline constexpr uint32_t Magic = 0x47574d56;
inline constexpr uint32_t Version = 1;
inline constexpr size_t MaxWindows = 64;
inline constexpr size_t MessageChars = 512;

enum class Operation : uint32_t { Ping, Register, Initialize, State, Hide, Show, Release };
enum class State : uint32_t {
    Checking, Installing, Initializing, Ready, Unavailable,
    Restoring, RestoreFailed, Conflict, RestartRequired, Stopped
};

#pragma pack(push, 1)
struct Request {
    uint32_t magic{Magic};
    uint32_t version{Version};
    Operation operation{Operation::Ping};
    uint32_t processId{}; // Must equal GetNamedPipeClientProcessId, authenticated by the broker.
    uint32_t count{};
    uint64_t window{};
    uint64_t windows[MaxWindows]{}; // Register whitelist, copied once from the startup HWND array.
};
struct Reply {
    uint32_t magic{Magic};
    uint32_t version{Version};
    uint32_t error{}; // Win32 operation error; State may still report initialization failure.
    uint32_t parkingCode{};
    State state{State::Checking};
    uint32_t installed{};
    uint32_t active{};
    uint32_t remaining{};
    uint32_t hiddenCount{};
    uint64_t hidden[MaxWindows]{};
    int32_t left{}, top{}, right{}, bottom{};
    wchar_t message[MessageChars]{};
};
#pragma pack(pop)
static_assert(sizeof(wchar_t) == 2);
static_assert(sizeof(Request) == 540);
static_assert(sizeof(Reply) == 1588);

// Restricted namespace derives from the current logon SID + Windows session ID.
// These names contain no caller-controlled path or arbitrary command line.
std::wstring BrokerPipeName();
std::wstring BrokerMutexName();
std::wstring RuntimeDirectory(); // Per-user %LocalAppData%\GameWindowMapper\Runtime\v1
}
