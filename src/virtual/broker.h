#pragma once

#include "display_manager.h"
#include <functional>
#include <string>

namespace gwm::vd {

struct BrokerOptions {
    // Empty functions select the production implementation. Native tests inject
    // read-only/fake display operations and never install or reconfigure drivers.
    std::function<DisplayResult()> inspect;
    std::function<DisplayResult(int, int, const PhaseCallback&)> ensure;
    std::function<DisplayResult()> detach;
    std::function<DisplayResult(const PhaseCallback&)> install;
    // In-process test isolation only: never accepted as a helper CLI argument.
    std::wstring instanceSuffix;
    HANDLE readyEvent{};
    HANDLE stopEvent{};
    DWORD startupIdleMs{15000};
    DWORD finalReplyGraceMs{2000};
    DWORD testOperationDelayMs{}; // In-process tests only; not a helper CLI option.
};

// Both entry points run in the native x64 helper, not inside the x86 DLL.
// Exit codes are Win32 errors. An existing broker is a successful no-op.
DWORD RunRecoveryBroker(BrokerOptions options = {});
DWORD RunDriverInstaller();

} // namespace gwm::vd
