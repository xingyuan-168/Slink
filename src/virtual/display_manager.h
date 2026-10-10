#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace gwm::vd {

// The native x64 helper owns this API. No function changes process DPI awareness,
// elevates itself, installs a trust certificate, or reboots Windows.
enum class Phase { Checking, VerifyingPackage, Installing, Initializing, Ready };
using PhaseCallback = std::function<void(Phase, const std::wstring&)>;

struct DisplayResult {
    DWORD code{ERROR_SUCCESS};
    std::wstring message;
    bool rebootRequired{};
    bool installed{};
    bool active{};
    RECT bounds{};
    std::wstring instanceId;
    std::wstring gdiDeviceName;
    explicit operator bool() const noexcept { return code == ERROR_SUCCESS && !rebootRequired; }
};

// Read-only. An installed, healthy, detached device is success with active=false.
// ERROR_NOT_FOUND means there is no owned device. ERROR_ALREADY_EXISTS means an
// unowned MttVDD device/configuration conflicts with this integration.
DisplayResult InspectOwnedDisplay();

// Read-only preflight for the fixed release. Safe before asking for UAC; the
// installer independently verifies again after elevation and protective staging.
DisplayResult VerifyDriverPackage(const std::wstring& packageDirectory);

// Requires an elevated AMD64 process. packageDirectory must contain the original
// 25.7.23 release's MttVDD.inf, MttVDD.dll and mttvdd.cat. Their pinned SHA256 and
// Authenticode trust are checked before Windows is allowed to install anything.
// Never edits these files; driver configuration is generated separately.
// System publisher confirmation is allowed on the first installation. Set
// allowSystemPrompt=false only for a diagnostic that must fail without any UI.
DisplayResult InstallOwnedDriver(const std::wstring& packageDirectory,
                                 const PhaseCallback& phase = {}, bool allowSystemPrompt = true);

// Current console session only. Keeps every other path's geometry/mode and does
// not select a new primary display. It never shrinks an already active screen.
// All dimensions and returned bounds are physical pixels. Wait is bounded to
// 10 seconds for a newly installed monitor; callbacks run on the caller thread.
DisplayResult EnsureExtendedDisplay(int minWidth = 1920, int minHeight = 1080,
                                    const PhaseCallback& phase = {});

// Caller MUST restore all parked windows and release all session leases first.
// Removes only the owned path from the *current* topology, never an old snapshot.
// The device/driver package stays installed. Idempotent when already detached.
DisplayResult DetachOwnedDisplay();

// %ProgramData%\GameWindowMapper\VirtualDisplay. No filesystem changes.
std::wstring ConfigurationDirectory();

} // namespace gwm::vd
