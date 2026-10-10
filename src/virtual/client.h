#pragma once
#include "protocol.h"
#include <span>
#include <optional>

namespace gwm::vd {
// Use on one DLL-owned worker thread. No graphics or host/UI pointer crosses IPC.
// Register copies the immutable startup HWND whitelist into the recovery broker.
class RecoveryClient {
public:
    DWORD connect(HINSTANCE module, std::span<const uint64_t> windows);
    ipc::Reply initialize();
    ipc::Reply query();
    ipc::Reply hide(uint64_t window);
    ipc::Reply show(uint64_t window);
    ipc::Reply release();
    bool registered() const noexcept { return registered_; }
    const ipc::Reply& lastReply() const noexcept { return lastReply_; }
private:
    ipc::Reply call(ipc::Request request, DWORD timeoutMs = 10000);
    ipc::Reply exchange(ipc::Request request, DWORD timeoutMs);
    ipc::Reply operation(ipc::Operation operation, uint64_t window = 0, DWORD timeoutMs = 10000);
    std::wstring helper_;
    bool registered_{};
    ipc::Reply lastReply_{};
    std::optional<ipc::Request> pending_;
};
}
