#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gwm::vd {

struct SessionIdentity {
    uint32_t processId{};
    uint64_t creationTime{};
    bool operator==(const SessionIdentity&) const = default;
};

enum class ParkingCode : uint32_t {
    ok, alreadyHidden, notHidden, sourceGone,
    invalidArgument, invalidSession, sourceUnavailable, accessDenied,
    ownedByAnotherSession, unsupportedWindow, fullscreenUnsupported,
    targetUnavailable, targetTooSmall, identityUnverifiable,
    moveFailed, restoreFailed, timedOut, persistenceFailed, invalidRecoveryRecord
};

struct ParkingResult {
    ParkingCode code{ParkingCode::ok};
    DWORD win32Error{};
    uint64_t window{};
    bool recordRetained{};
    explicit operator bool() const noexcept {
        return code == ParkingCode::ok || code == ParkingCode::alreadyHidden ||
            code == ParkingCode::notHidden || code == ParkingCode::sourceGone;
    }
};

// Persist fields explicitly in a versioned journal; this is not a packed wire ABI.
// The property cookie is a 32-bit value so the same record protects x86/x64 HWNDs.
// The journal must be ACL restricted to the current user and validated on import.
enum class RecordPurpose : uint32_t { Hidden, DisplayTransition };

struct ParkingRecord {
    uint32_t version{2};
    RecordPurpose purpose{RecordPurpose::Hidden};
    SessionIdentity session{};
    uint64_t window{};
    uint32_t sourceProcessId{};
    uint32_t sourceThreadId{};
    uint64_t sourceCreationTime{};
    uint32_t windowClassAtom{};
    uint32_t lifecycleCookie{};
    WINDOWPLACEMENT originalPlacement{};
    RECT originalRect{};           // Physical screen coordinates, unlike WINDOWPLACEMENT.
    RECT originalMonitorRect{};
    RECT originalWorkRect{};
    RECT virtualBounds{};
    wchar_t originalMonitorName[CCHDEVICENAME]{};
    wchar_t originalMonitorIdentity[128]{};
    wchar_t virtualMonitorName[CCHDEVICENAME]{};
    wchar_t virtualMonitorIdentity[128]{};
    uint32_t originalDpi{};
    bool toolWindow{};
};

struct RestoreReport {
    std::vector<ParkingResult> results;
    size_t remaining{};
    bool complete() const noexcept { return remaining == 0; }
};

struct ParkingOptions {
    DWORD operationTimeoutMs{1500};
    // Called before moving a newly owned HWND and before forgetting any record.
    // Return only after durable replacement succeeds. Failure preserves records.
    // Runs under the module's mutex: do not reenter WindowParking from this callback.
    // An empty callback is useful for isolated tests; production must provide one.
    std::function<bool(std::span<const ParkingRecord>)> persist;
};

class WindowParking final {
public:
    explicit WindowParking(ParkingOptions options = {});
    ~WindowParking(); // No implicit restoration: callers must inspect restoreAll().
    WindowParking(const WindowParking&) = delete;
    WindowParking& operator=(const WindowParking&) = delete;

    // Caller authenticates the IPC peer. Handle needs SYNCHRONIZE and
    // PROCESS_QUERY_LIMITED_INFORMATION; a private duplicate is retained.
    ParkingResult registerSession(HANDLE process, SessionIdentity& session);
    ParkingResult unregisterSession(const SessionIdentity& session);
    bool sessionAlive(const SessionIdentity& session) const;

    // Controller must prove this is its owned active virtual display. Bounds must
    // be wholly contained in one currently active monitor, never off-desktop.
    ParkingResult hide(const SessionIdentity& session, HWND window, const RECT& virtualBounds);
    // Save a whitelisted visible window before enabling an inactive display.
    // These durable checkpoints are not hidden-window records. Complete them
    // after the display change, and retain failed restores for normal recovery.
    ParkingResult checkpoint(const SessionIdentity& session, HWND window);
    RestoreReport restoreCheckpoints();
    ParkingResult show(const SessionIdentity& session, HWND window);
    RestoreReport restoreSession(const SessionIdentity& session);
    RestoreReport restoreAll();
    RestoreReport restoreExitedSessions();
    bool isHidden(HWND window) const;
    bool owns(const SessionIdentity& session, HWND window) const;

    std::vector<ParkingRecord> snapshot() const;
    // Intended for an ACL-protected recovery journal, not untrusted IPC data.
    // Imports only into an empty instance. Original session may already be dead;
    // source identity/cookie is revalidated before every restore. Does not move.
    ParkingResult importRecords(std::span<const ParkingRecord> records);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const wchar_t* parkingCodeText(ParkingCode code) noexcept;

} // namespace gwm::vd
