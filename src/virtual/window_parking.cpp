#include "window_parking.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace gwm::vd {
namespace {
constexpr wchar_t kCookieProperty[] = L"GameWindowMapper.Parking.Owner.v1";
constexpr DWORD kPositionFlags = SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE |
    SWP_NOZORDER | SWP_NOOWNERZORDER;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Handle old(std::exchange(value_, std::exchange(other.value_, nullptr))); }
        return *this;
    }
    Handle(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_{};
};

class PhysicalCoordinates {
public:
    PhysicalCoordinates() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinates() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
private:
    DPI_AWARENESS_CONTEXT previous_{};
};

class CookieGuard {
public:
    CookieGuard(HWND window, uint32_t cookie) : window_(window), cookie_(cookie) {}
    ~CookieGuard() {
        if (!retained && reinterpret_cast<uintptr_t>(GetPropW(window_, kCookieProperty)) == cookie_)
            RemovePropW(window_, kCookieProperty);
    }
    bool retained{};
private:
    HWND window_{};
    uint32_t cookie_{};
};

uint64_t windowValue(HWND window) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window)); }
HWND windowHandle(uint64_t window) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(window)); }
uint64_t fileTimeValue(const FILETIME& value) {
    return static_cast<uint64_t>(value.dwLowDateTime) | (static_cast<uint64_t>(value.dwHighDateTime) << 32);
}
bool creationTime(HANDLE process, uint64_t& value) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    value = fileTimeValue(created);
    return true;
}
bool positive(const RECT& rect) {
    const auto width = int64_t(rect.right) - rect.left, height = int64_t(rect.bottom) - rect.top;
    return width > 0 && height > 0 && width <= INT_MAX && height <= INT_MAX;
}
bool contains(const RECT& outer, const RECT& inner) {
    return positive(inner) && inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}
bool sameRect(const RECT& a, const RECT& b, LONG tolerance = 1) {
    return std::abs(int64_t(a.left) - b.left) <= tolerance &&
        std::abs(int64_t(a.top) - b.top) <= tolerance &&
        std::abs(int64_t(a.right) - b.right) <= tolerance &&
        std::abs(int64_t(a.bottom) - b.bottom) <= tolerance;
}
ParkingResult result(ParkingCode code, HWND window = nullptr, DWORD error = 0, bool retained = false) {
    return {code, error, windowValue(window), retained};
}
ParkingCode apiFailure(DWORD error, ParkingCode fallback) {
    return error == ERROR_ACCESS_DENIED ? ParkingCode::accessDenied : fallback;
}

struct Monitor {
    HMONITOR handle{};
    MONITORINFOEXW info{};
    std::wstring identity;
};
Monitor readMonitor(HMONITOR handle) {
    Monitor monitor; monitor.handle = handle; monitor.info.cbSize = sizeof(monitor.info);
    if (!handle || !GetMonitorInfoW(handle, &monitor.info)) return {};
    DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
    if (EnumDisplayDevicesW(monitor.info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
        monitor.identity = device.DeviceID;
    return monitor;
}
BOOL CALLBACK collectMonitors(HMONITOR handle, HDC, LPRECT, LPARAM parameter) {
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(parameter);
    auto monitor = readMonitor(handle);
    if (monitor.handle) monitors.push_back(std::move(monitor));
    return TRUE;
}
std::vector<Monitor> monitors() {
    std::vector<Monitor> list;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&list));
    return list;
}
bool monitorMatches(const ParkingRecord& record, const Monitor& monitor) {
    if (record.originalMonitorIdentity[0])
        return !monitor.identity.empty() && _wcsicmp(record.originalMonitorIdentity, monitor.identity.c_str()) == 0;
    // Without a stable interface name, do not follow a recycled DISPLAYn to a
    // different location. Falling back to a visible primary monitor is safer.
    return _wcsicmp(record.originalMonitorName, monitor.info.szDevice) == 0 &&
        sameRect(record.originalMonitorRect, monitor.info.rcMonitor, 0);
}
bool parkingMonitorMatches(const ParkingRecord& record, const Monitor& monitor) {
    if (record.virtualMonitorIdentity[0] && !monitor.identity.empty())
        return _wcsicmp(record.virtualMonitorIdentity, monitor.identity.c_str()) == 0;
    return _wcsicmp(record.virtualMonitorName, monitor.info.szDevice) == 0 ||
        contains(monitor.info.rcMonitor, record.virtualBounds);
}
RECT fitVisible(RECT rect, const RECT& work) {
    const LONG width = std::min(rect.right - rect.left, work.right - work.left);
    const LONG height = std::min(rect.bottom - rect.top, work.bottom - work.top);
    rect.left = std::clamp(rect.left, work.left, work.right - width);
    rect.top = std::clamp(rect.top, work.top, work.bottom - height);
    rect.right = rect.left + width; rect.bottom = rect.top + height;
    return rect;
}
RECT workspaceToScreen(RECT rect, const RECT& monitor, const RECT& work, bool tool) {
    if (!tool) OffsetRect(&rect, work.left - monitor.left, work.top - monitor.top);
    return rect;
}
RECT screenToWorkspace(RECT rect, const Monitor& monitor, bool tool) {
    if (!tool) OffsetRect(&rect, monitor.info.rcMonitor.left - monitor.info.rcWork.left,
        monitor.info.rcMonitor.top - monitor.info.rcWork.top);
    return rect;
}
bool topLevelMovable(HWND window) {
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    return !(style & WS_CHILD) && IsWindowVisible(window) && GetAncestor(window, GA_ROOT) == window;
}
bool fullscreenLike(HWND window, const RECT& rect, const Monitor& monitor) {
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    return !IsZoomed(window) && !(style & (WS_CAPTION | WS_THICKFRAME)) &&
        contains(rect, monitor.info.rcMonitor);
}

enum class Identity { same, gone, unverifiable };
Identity identify(const ParkingRecord& record, DWORD& error) {
    const auto window = windowHandle(record.window);
    if (!IsWindow(window)) return Identity::gone;
    DWORD pid{};
    const DWORD tid = GetWindowThreadProcessId(window, &pid);
    if (pid != record.sourceProcessId || tid != record.sourceThreadId ||
        static_cast<uint32_t>(GetClassLongPtrW(window, GCW_ATOM)) != record.windowClassAtom)
        return Identity::gone;
    if (reinterpret_cast<uintptr_t>(GetPropW(window, kCookieProperty)) != record.lifecycleCookie)
        return Identity::gone;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.get()) { error = GetLastError(); return Identity::unverifiable; }
    uint64_t created{};
    if (!creationTime(process.get(), created)) { error = GetLastError(); return Identity::unverifiable; }
    return created == record.sourceCreationTime ? Identity::same : Identity::gone;
}
DWORD remainingTime(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return deadline > now ? static_cast<DWORD>(deadline - now) : 0;
}
bool responsive(HWND window, ULONGLONG deadline) {
    const DWORD remaining = remainingTime(deadline);
    if (!remaining) return false;
    DWORD_PTR ignored{};
    return SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
        std::min<DWORD>(remaining, 100), &ignored) != 0;
}

// Verification stays bounded even when the target owns a hung input queue.
// Require stable observations and a responsive target; a queued async request
// alone is never sufficient to forget the restoration record.
template<class Predicate>
bool awaitWindow(const ParkingRecord& record, ULONGLONG deadline, Predicate predicate, DWORD stableMs = 80) {
    ULONGLONG stableSince{};
    do {
        DWORD ignored{};
        if (identify(record, ignored) != Identity::same) return false;
        if (predicate()) {
            if (!stableSince) stableSince = GetTickCount64();
            if (GetTickCount64() - stableSince >= stableMs && responsive(windowHandle(record.window), deadline)) return true;
        } else stableSince = 0;
        const DWORD remaining = remainingTime(deadline);
        if (!remaining) break;
        Sleep(std::min<DWORD>(10, remaining));
    } while (remainingTime(deadline));
    return false;
}
bool validRecord(const ParkingRecord& record) {
    return (record.version == 1 || record.version == 2) &&
        (record.purpose == RecordPurpose::Hidden || (record.version == 2 && record.purpose == RecordPurpose::DisplayTransition)) &&
        record.session.processId && record.session.creationTime &&
        record.window && record.window <= UINTPTR_MAX && record.sourceProcessId &&
        record.sourceThreadId && record.sourceCreationTime && record.windowClassAtom && record.lifecycleCookie &&
        record.originalPlacement.length == sizeof(WINDOWPLACEMENT) &&
        (record.originalPlacement.showCmd == SW_SHOWNORMAL || record.originalPlacement.showCmd == SW_SHOWMAXIMIZED ||
            record.originalPlacement.showCmd == SW_SHOWMINIMIZED) &&
        positive(record.originalPlacement.rcNormalPosition) && positive(record.originalRect) &&
        positive(record.originalMonitorRect) && positive(record.originalWorkRect) &&
        (record.purpose == RecordPurpose::DisplayTransition || positive(record.virtualBounds)) &&
        std::wmemchr(record.originalMonitorName, 0, std::size(record.originalMonitorName)) &&
        std::wmemchr(record.originalMonitorIdentity, 0, std::size(record.originalMonitorIdentity)) &&
        std::wmemchr(record.virtualMonitorName, 0, std::size(record.virtualMonitorName)) &&
        std::wmemchr(record.virtualMonitorIdentity, 0, std::size(record.virtualMonitorIdentity));
}
} // namespace

struct WindowParking::Impl {
    struct Session { SessionIdentity identity; Handle process; };
    ParkingOptions options;
    mutable std::mutex mutex;
    std::vector<Session> sessions;
    std::vector<ParkingRecord> records;

    explicit Impl(ParkingOptions value) : options(std::move(value)) {
        options.operationTimeoutMs = std::clamp<DWORD>(options.operationTimeoutMs, 200, 10000);
    }
    auto findSession(const SessionIdentity& identity) {
        return std::find_if(sessions.begin(), sessions.end(), [&](const Session& entry) { return entry.identity == identity; });
    }
    auto findRecord(HWND window) {
        return std::find_if(records.begin(), records.end(), [&](const ParkingRecord& entry) { return entry.window == windowValue(window); });
    }
    bool alive(const SessionIdentity& identity) {
        const auto session = findSession(identity);
        return session != sessions.end() && WaitForSingleObject(session->process.get(), 0) == WAIT_TIMEOUT;
    }
    bool persist(std::span<const ParkingRecord> value) {
        if (!options.persist) return true;
        try { return options.persist(value); } catch (...) { return false; }
    }
    ParkingResult forget(HWND window, ParkingCode code) {
        const auto entry = findRecord(window);
        if (entry == records.end()) return result(code, window);
        auto next = records;
        next.erase(next.begin() + std::distance(records.begin(), entry));
        if (!persist(next)) return result(ParkingCode::persistenceFailed, window, ERROR_WRITE_FAULT, true);
        DWORD ignored{};
        if (identify(*entry, ignored) == Identity::same) RemovePropW(window, kCookieProperty);
        records.swap(next);
        return result(code, window);
    }
    ParkingResult verifyFailure(const ParkingRecord& record, ParkingCode code, DWORD error) {
        DWORD identityError{};
        const auto identity = identify(record, identityError);
        if (identity == Identity::gone) return forget(windowHandle(record.window), ParkingCode::sourceGone);
        if (identity == Identity::unverifiable) return result(ParkingCode::identityUnverifiable,
            windowHandle(record.window), identityError, true);
        return result(apiFailure(error, code), windowHandle(record.window), error, true);
    }
    ParkingResult remember(const SessionIdentity& session, HWND window, const RECT& bounds, RecordPurpose purpose) {
        if (!alive(session)) return result(ParkingCode::invalidSession, window);
        const bool checkpoint = purpose == RecordPurpose::DisplayTransition;
        if (!window || (!checkpoint && !positive(bounds))) return result(ParkingCode::invalidArgument, window);
        const auto existing = findRecord(window);
        if (existing != records.end()) {
            DWORD error{};
            const auto identity = identify(*existing, error);
            if (identity == Identity::gone) {
                const auto removed = forget(window, ParkingCode::sourceGone);
                if (!removed) return removed;
            } else {
                if (identity == Identity::unverifiable) return result(ParkingCode::identityUnverifiable, window, error, true);
                return result(existing->session == session ? ParkingCode::alreadyHidden : ParkingCode::ownedByAnotherSession, window, 0, true);
            }
        }
        if (!IsWindow(window)) return result(ParkingCode::sourceUnavailable, window, ERROR_INVALID_WINDOW_HANDLE);
        if (!topLevelMovable(window)) return result(ParkingCode::unsupportedWindow, window);
        Monitor target;
        if (!checkpoint) {
            target = readMonitor(MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL));
            if (!target.handle || !contains(target.info.rcMonitor, bounds)) return result(ParkingCode::targetUnavailable, window);
        }
        if (GetPropW(window, kCookieProperty)) return result(ParkingCode::ownedByAnotherSession, window);
        ParkingRecord record; record.purpose = purpose; record.session = session;
        record.window = windowValue(window); record.virtualBounds = bounds;
        if (target.handle) {
            wcscpy_s(record.virtualMonitorName, target.info.szDevice);
            wcsncpy_s(record.virtualMonitorIdentity, target.identity.c_str(), _TRUNCATE);
        }
        DWORD pid{};
        record.sourceThreadId = GetWindowThreadProcessId(window, &pid); record.sourceProcessId = pid;
        Handle source(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!source.get() || !creationTime(source.get(), record.sourceCreationTime)) {
            const auto error = GetLastError(); return result(apiFailure(error, ParkingCode::identityUnverifiable), window, error);
        }
        record.windowClassAtom = static_cast<uint32_t>(GetClassLongPtrW(window, GCW_ATOM));
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&record.lifecycleCookie), sizeof(record.lifecycleCookie), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return result(ParkingCode::moveFailed, window, ERROR_GEN_FAILURE);
        if (!record.lifecycleCookie) record.lifecycleCookie = 1;
        records.reserve(records.size() + 1);
        if (!SetPropW(window, kCookieProperty, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(record.lifecycleCookie)))) {
            const auto error = GetLastError(); return result(apiFailure(error, ParkingCode::moveFailed), window, error);
        }
        CookieGuard cookieGuard(window, record.lifecycleCookie);
        record.originalPlacement.length = sizeof(record.originalPlacement);
        if (!GetWindowPlacement(window, &record.originalPlacement) || !GetWindowRect(window, &record.originalRect))
            return result(ParkingCode::sourceUnavailable, window, GetLastError());
        const auto origin = readMonitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
        if (!origin.handle) return result(ParkingCode::targetUnavailable, window);
        if (fullscreenLike(window, record.originalRect, origin)) return result(ParkingCode::fullscreenUnsupported, window);
        record.originalMonitorRect = origin.info.rcMonitor; record.originalWorkRect = origin.info.rcWork;
        wcscpy_s(record.originalMonitorName, origin.info.szDevice);
        wcsncpy_s(record.originalMonitorIdentity, origin.identity.c_str(), _TRUNCATE);
        record.originalDpi = GetDpiForWindow(window);
        record.toolWindow = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
        const RECT normal = record.originalPlacement.showCmd == SW_SHOWNORMAL ? record.originalRect :
            workspaceToScreen(record.originalPlacement.rcNormalPosition, record.originalMonitorRect, record.originalWorkRect, record.toolWindow);
        if (!positive(normal)) return result(ParkingCode::unsupportedWindow, window);
        if (!checkpoint && (normal.right - normal.left > bounds.right - bounds.left || normal.bottom - normal.top > bounds.bottom - bounds.top))
            return result(ParkingCode::targetTooSmall, window);
        DWORD error{};
        if (identify(record, error) != Identity::same) return result(ParkingCode::sourceUnavailable, window, error);
        records.push_back(record);
        if (!persist(records)) { records.pop_back(); return result(ParkingCode::persistenceFailed, window, ERROR_WRITE_FAULT); }
        cookieGuard.retained = true;
        return result(ParkingCode::ok, window, 0, true);
    }
    ParkingResult restore(HWND window) {
        const auto entry = findRecord(window);
        if (entry == records.end()) return result(ParkingCode::notHidden, window);
        // Work with a copy: successful cleanup replaces the backing vector.
        const ParkingRecord record = *entry;
        DWORD identityError{};
        const auto identity = identify(record, identityError);
        if (identity == Identity::gone) return forget(window, ParkingCode::sourceGone);
        if (identity == Identity::unverifiable) return result(ParkingCode::identityUnverifiable, window, identityError, true);

        const auto displays = monitors();
        auto target = std::find_if(displays.begin(), displays.end(), [&](const Monitor& monitor) { return monitorMatches(record, monitor); });
        const bool originalAvailable = target != displays.end();
        if (!originalAvailable) {
            // Never select this record's parking screen as its recovery fallback.
            target = std::find_if(displays.begin(), displays.end(), [&](const Monitor& monitor) {
                return (monitor.info.dwFlags & MONITORINFOF_PRIMARY) && !parkingMonitorMatches(record, monitor);
            });
            if (target == displays.end()) target = std::find_if(displays.begin(), displays.end(), [&](const Monitor& monitor) {
                return !parkingMonitorMatches(record, monitor);
            });
            if (target == displays.end()) return result(ParkingCode::targetUnavailable, window, ERROR_NOT_READY, true);
        }

        auto placement = record.originalPlacement;
        placement.length = sizeof(placement);
        placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
        const bool originalNormal = placement.showCmd == SW_SHOWNORMAL;
        RECT expected = originalNormal ? record.originalRect : workspaceToScreen(placement.rcNormalPosition,
            record.originalMonitorRect, record.originalWorkRect, record.toolWindow);
        const bool sameTopology = originalAvailable && sameRect(target->info.rcMonitor, record.originalMonitorRect, 0) &&
            sameRect(target->info.rcWork, record.originalWorkRect, 0);
        if (!sameTopology) {
            // Preserve the saved offset on a moved original display; otherwise
            // choose a visible rectangle on the current physical primary.
            if (originalAvailable) OffsetRect(&expected,
                target->info.rcMonitor.left - record.originalMonitorRect.left,
                target->info.rcMonitor.top - record.originalMonitorRect.top);
            expected = fitVisible(expected, target->info.rcWork);
            placement.rcNormalPosition = screenToWorkspace(expected, *target, record.toolWindow);
            placement.ptMinPosition = {-1, -1}; placement.ptMaxPosition = {-1, -1};
            placement.flags &= ~WPF_SETMINPOSITION;
        }
        if (originalNormal) placement.showCmd = SW_SHOWNOACTIVATE;
        else if (placement.showCmd == SW_SHOWMINIMIZED) placement.showCmd = SW_SHOWMINNOACTIVE;

        const ULONGLONG deadline = GetTickCount64() + options.operationTimeoutMs;
        const auto stateMatches = [&] {
            if (record.originalPlacement.showCmd == SW_SHOWMINIMIZED) return IsIconic(window) != FALSE;
            if (record.originalPlacement.showCmd == SW_SHOWMAXIMIZED) return IsZoomed(window) != FALSE &&
                MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == target->handle;
            RECT actual{};
            return !IsIconic(window) && !IsZoomed(window) && GetWindowRect(window, &actual) && sameRect(actual, expected);
        };
        const auto checkpointMatches = [&] {
            WINDOWPLACEMENT actual{}; actual.length = sizeof(actual);
            return stateMatches() && GetWindowPlacement(window, &actual) && sameRect(actual.rcNormalPosition, placement.rcNormalPosition, 2);
        };
        // An unchanged checkpoint needs verification, not a show operation that
        // could activate a maximized source during ordinary canvas startup.
        if (record.purpose == RecordPurpose::DisplayTransition && checkpointMatches() &&
            awaitWindow(record, std::min<ULONGLONG>(deadline, GetTickCount64() + 350), checkpointMatches, 300))
            return forget(window, ParkingCode::ok);
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowPlacement(window, &placement)) return verifyFailure(record, ParkingCode::restoreFailed, GetLastError());
        const bool stateRestored = awaitWindow(record, deadline, stateMatches,
            record.purpose == RecordPurpose::DisplayTransition ? 300 : 80);
        if (!stateRestored) return verifyFailure(record, ParkingCode::restoreFailed, ERROR_TIMEOUT);

        // A minimized HWND's current RECT is an icon position, so verify the
        // stored restore rectangle rather than incorrectly comparing that RECT.
        WINDOWPLACEMENT actual{}; actual.length = sizeof(actual);
        if (!GetWindowPlacement(window, &actual) || !sameRect(actual.rcNormalPosition, placement.rcNormalPosition, 2))
            return verifyFailure(record, ParkingCode::restoreFailed, ERROR_INVALID_DATA);
        return forget(window, ParkingCode::ok);
    }
    RestoreReport restoreSelected(const SessionIdentity* session, bool exitedOnly, bool checkpointsOnly = false) {
        RestoreReport report;
        std::vector<HWND> targets;
        for (const auto& entry : records) if ((!session || entry.session == *session) && (!exitedOnly || !alive(entry.session)) &&
            (!checkpointsOnly || entry.purpose == RecordPurpose::DisplayTransition))
            targets.push_back(windowHandle(entry.window));
        for (const auto window : targets) report.results.push_back(restore(window));
        for (const auto& entry : records) if ((!session || entry.session == *session) && (!exitedOnly || !alive(entry.session)) &&
            (!checkpointsOnly || entry.purpose == RecordPurpose::DisplayTransition)) ++report.remaining;
        return report;
    }
};

WindowParking::WindowParking(ParkingOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
WindowParking::~WindowParking() = default;

ParkingResult WindowParking::registerSession(HANDLE process, SessionIdentity& identity) {
    std::scoped_lock lock(impl_->mutex);
    SessionIdentity candidate{}; candidate.processId = GetProcessId(process);
    if (!candidate.processId || !creationTime(process, candidate.creationTime) || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        return result(ParkingCode::invalidSession, nullptr, GetLastError());
    if (impl_->findSession(candidate) != impl_->sessions.end()) { identity = candidate; return {}; }
    HANDLE duplicate{};
    if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return result(ParkingCode::invalidSession, nullptr, GetLastError());
    impl_->sessions.push_back({candidate, Handle(duplicate)});
    identity = candidate;
    return {};
}
ParkingResult WindowParking::unregisterSession(const SessionIdentity& session) {
    std::scoped_lock lock(impl_->mutex);
    if (std::any_of(impl_->records.begin(), impl_->records.end(), [&](const ParkingRecord& entry) { return entry.session == session; }))
        return result(ParkingCode::restoreFailed, nullptr, ERROR_BUSY, true);
    const auto found = impl_->findSession(session);
    if (found != impl_->sessions.end()) impl_->sessions.erase(found);
    return {};
}
bool WindowParking::sessionAlive(const SessionIdentity& session) const {
    std::scoped_lock lock(impl_->mutex); return impl_->alive(session);
}

ParkingResult WindowParking::hide(const SessionIdentity& session, HWND window, const RECT& bounds) {
    std::scoped_lock lock(impl_->mutex);
    PhysicalCoordinates coordinates;
    const auto remembered = impl_->remember(session, window, bounds, RecordPurpose::Hidden);
    if (!remembered || remembered.code != ParkingCode::ok) return remembered;
    const ParkingRecord record = *impl_->findRecord(window);
    const auto target = readMonitor(MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL));
    if (!target.handle || !contains(target.info.rcMonitor, bounds)) {
        const auto restored = impl_->restore(window);
        return result(ParkingCode::targetUnavailable, window, ERROR_NOT_READY, !restored || restored.recordRetained);
    }
    const RECT normal = record.originalPlacement.showCmd == SW_SHOWNORMAL ? record.originalRect :
        workspaceToScreen(record.originalPlacement.rcNormalPosition, record.originalMonitorRect, record.originalWorkRect, record.toolWindow);
    const LONG width = normal.right - normal.left, height = normal.bottom - normal.top;
    RECT parked{bounds.left + (bounds.right - bounds.left - width) / 2,
        bounds.top + (bounds.bottom - bounds.top - height) / 2, 0, 0};
    parked.right = parked.left + width; parked.bottom = parked.top + height;

    const ULONGLONG deadline = GetTickCount64() + impl_->options.operationTimeoutMs;
    // SetWindowPlacement uses workspace coordinates and can restore a minimized
    // or maximized source without foreground activation. Position verification
    // still uses GetWindowRect under per-monitor thread awareness.
    WINDOWPLACEMENT placement = record.originalPlacement;
    placement.flags = WPF_ASYNCWINDOWPLACEMENT;
    placement.showCmd = SW_SHOWNOACTIVATE;
    placement.rcNormalPosition = screenToWorkspace(parked, target, record.toolWindow);
    placement.ptMinPosition = {-1, -1}; placement.ptMaxPosition = {-1, -1};
    SetLastError(ERROR_SUCCESS);
    const bool placed = SetWindowPlacement(window, &placement) != FALSE;
    DWORD moveError = placed ? ERROR_SUCCESS : GetLastError();
    bool moved = placed && awaitWindow(record, deadline, [&] { return !IsIconic(window) && !IsZoomed(window); });
    if (moved) {
        // Explicit physical dimensions defeat DPI-induced resize and ensure the
        // complete outer window fits. No source styles or z-order are changed.
        SetLastError(ERROR_SUCCESS);
        moved = SetWindowPos(window, nullptr, parked.left, parked.top, width, height, kPositionFlags) != FALSE;
        moveError = moved ? ERROR_SUCCESS : GetLastError();
        if (moved) moved = awaitWindow(record, deadline, [&] {
            RECT actual{};
            return !IsIconic(window) && !IsZoomed(window) && GetWindowRect(window, &actual) &&
                sameRect(actual, parked) && contains(bounds, actual);
        });
    }
    if (moved) return result(ParkingCode::ok, window, 0, true);
    if (!moveError) moveError = ERROR_TIMEOUT;
    const auto failure = impl_->verifyFailure(record, ParkingCode::moveFailed, moveError);
    if (failure.code == ParkingCode::sourceGone) return failure;
    const auto restored = impl_->restore(window);
    // Failed parking rolls back immediately. A failed rollback deliberately
    // retains the record and prevents the controller withdrawing the display.
    return result(apiFailure(moveError, ParkingCode::moveFailed), window, moveError, !restored || restored.recordRetained);
}

ParkingResult WindowParking::checkpoint(const SessionIdentity& session, HWND window) {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    return impl_->remember(session, window, {}, RecordPurpose::DisplayTransition);
}
RestoreReport WindowParking::restoreCheckpoints() {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    return impl_->restoreSelected(nullptr, false, true);
}

ParkingResult WindowParking::show(const SessionIdentity& session, HWND window) {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    const auto entry = impl_->findRecord(window);
    if (entry == impl_->records.end()) return result(ParkingCode::notHidden, window);
    if (!(entry->session == session)) return result(ParkingCode::ownedByAnotherSession, window, 0, true);
    return impl_->restore(window);
}
RestoreReport WindowParking::restoreSession(const SessionIdentity& session) {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    return impl_->restoreSelected(&session, false);
}
RestoreReport WindowParking::restoreAll() {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    return impl_->restoreSelected(nullptr, false);
}
RestoreReport WindowParking::restoreExitedSessions() {
    std::scoped_lock lock(impl_->mutex); PhysicalCoordinates coordinates;
    return impl_->restoreSelected(nullptr, true);
}
bool WindowParking::isHidden(HWND window) const {
    std::scoped_lock lock(impl_->mutex);
    const auto entry = impl_->findRecord(window); DWORD ignored{};
    return entry != impl_->records.end() && entry->purpose == RecordPurpose::Hidden && identify(*entry, ignored) != Identity::gone;
}
bool WindowParking::owns(const SessionIdentity& session, HWND window) const {
    std::scoped_lock lock(impl_->mutex);
    const auto entry = impl_->findRecord(window); DWORD ignored{};
    return entry != impl_->records.end() && entry->session == session && identify(*entry, ignored) != Identity::gone;
}
std::vector<ParkingRecord> WindowParking::snapshot() const {
    std::scoped_lock lock(impl_->mutex); return impl_->records;
}
ParkingResult WindowParking::importRecords(std::span<const ParkingRecord> records) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->records.empty()) return result(ParkingCode::invalidRecoveryRecord, nullptr, ERROR_BUSY, true);
    if (records.size() > 4096) return result(ParkingCode::invalidRecoveryRecord);
    std::vector<ParkingRecord> imported;
    imported.reserve(records.size());
    for (const auto& record : records) {
        if (!validRecord(record) || std::any_of(imported.begin(), imported.end(), [&](const ParkingRecord& entry) { return entry.window == record.window; }))
            return result(ParkingCode::invalidRecoveryRecord, windowHandle(record.window));
        // Preserve even unverifiable/stale records here. restore() safely prunes
        // destroyed HWNDs; inaccessible live windows remain available for retry.
        imported.push_back(record);
    }
    impl_->records.swap(imported);
    return {};
}

const wchar_t* parkingCodeText(ParkingCode code) noexcept {
    switch (code) {
    case ParkingCode::ok: return L"完成";
    case ParkingCode::alreadyHidden: return L"窗口已经隐藏";
    case ParkingCode::notHidden: return L"窗口未隐藏";
    case ParkingCode::sourceGone: return L"原窗口已关闭或身份已改变";
    case ParkingCode::invalidArgument: return L"窗口或显示区域参数无效";
    case ParkingCode::invalidSession: return L"宿主会话无效或已经退出";
    case ParkingCode::sourceUnavailable: return L"源窗口不可用";
    case ParkingCode::accessDenied: return L"权限不足，无法移动该窗口";
    case ParkingCode::ownedByAnotherSession: return L"窗口已由其他映射会话管理";
    case ParkingCode::unsupportedWindow: return L"此窗口不支持停放";
    case ParkingCode::fullscreenUnsupported: return L"全屏窗口不支持停放，请先切换为窗口模式";
    case ParkingCode::targetUnavailable: return L"可用的目标显示器不存在";
    case ParkingCode::targetTooSmall: return L"虚拟显示区域不能完整容纳源窗口";
    case ParkingCode::identityUnverifiable: return L"暂时无法确认窗口身份，恢复记录已保留";
    case ParkingCode::moveFailed: return L"源窗口未按请求移动";
    case ParkingCode::restoreFailed: return L"窗口恢复失败，请重试";
    case ParkingCode::timedOut: return L"源窗口响应超时";
    case ParkingCode::persistenceFailed: return L"无法保存窗口恢复记录";
    case ParkingCode::invalidRecoveryRecord: return L"窗口恢复记录无效";
    }
    return L"未知窗口停放错误";
}
} // namespace gwm::vd
