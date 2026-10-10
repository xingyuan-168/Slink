#include "controller.h"
#include <algorithm>

namespace gwm::vd {
namespace {
bool transportFault(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY || error == ERROR_TIMEOUT ||
        error == ERROR_NO_DATA || error == ERROR_NOT_FOUND;
}
ipc::Reply failed(DWORD error, ipc::Reply reply) {
    reply.error = error;
    reply.state = ipc::State::Unavailable;
    if (!reply.message[0]) _snwprintf_s(reply.message, _TRUNCATE,
        L"恢复辅助程序暂不可用（%lu），普通映射继续可用，请重新检查", error);
    return reply;
}
}
bool CanvasStatus::hidden(uint64_t window) const {
    return window && std::find(reply.hidden, reply.hidden + reply.hiddenCount, window) != reply.hidden + reply.hiddenCount;
}
CanvasController::~CanvasController() { shutdown(); }
void CanvasController::start(HINSTANCE module, std::vector<uint64_t> windows,
    std::function<void()> changed, std::function<void()> closed, std::function<void(uint64_t)> activate) {
    module_ = module; windows_ = std::move(windows);
    onChanged_ = std::move(changed); onClosed_ = std::move(closed); onActivate_ = std::move(activate);
    worker_ = std::thread([this] { run(); });
}
CanvasStatus CanvasController::snapshot() const { std::scoped_lock lock(mutex_); return status_; }
void CanvasController::enqueue(Command command) {
    {
        std::scoped_lock lock(mutex_); if (close_ || status_.finished || commands_.size() >= 64) return;
        if(command.action==Action::Activate){
            if(activating_==command.window||std::any_of(commands_.begin(),commands_.end(),[&](const Command& c){return c.action==Action::Activate&&c.window==command.window;}))return;
        }else if(status_.busy)return;
        commands_.push_back(command);status_.busy=true;
    }
    changed_.notify_one();if(onChanged_)onChanged_();
}
void CanvasController::hide(uint64_t window) { enqueue({Action::Hide, window}); }
void CanvasController::show(uint64_t window, bool activate) { enqueue({activate ? Action::Activate : Action::Show, window}); }
void CanvasController::retry() {
    { std::scoped_lock lock(mutex_); if (!close_) {commands_.clear();commands_.push_back({Action::Initialize});status_.busy=true;} }
    changed_.notify_one();
}
void CanvasController::close() {
    { std::scoped_lock lock(mutex_); if (status_.finished) return; close_ = status_.closing = true; commands_.clear(); }
    changed_.notify_one(); if (onChanged_) onChanged_();
}
void CanvasController::shutdown() {
    { std::scoped_lock lock(mutex_); shutdown_ = true; }
    close();
    if (worker_.joinable()) worker_.join();
}
void CanvasController::publish(const ipc::Reply& reply, bool busy, bool operation) {
    {
        std::scoped_lock lock(mutex_); status_.reply = reply; status_.busy = busy || !commands_.empty();
        if (operation) {
            status_.transient = reply.error && reply.error != ERROR_IO_PENDING ? reply.message : L"";
            status_.transientUntil = GetTickCount64() + 5000;
        }
    }
    if (onChanged_) onChanged_();
}
void CanvasController::finished() {
    { std::scoped_lock lock(mutex_); status_.finished = true; status_.busy = false; status_.reply = {}; status_.reply.state = ipc::State::Stopped; }
    if (onChanged_) onChanged_();
    if (onClosed_) onClosed_();
}
void CanvasController::run() noexcept {
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RecoveryClient client;
    bool possibleLease = false, connectNeeded = true, initialized = false;
    try {
        for (;;) {
            bool closing{}, shuttingDown{}; Command command{Action::Initialize}; bool hasCommand{};
            {
                std::unique_lock lock(mutex_);
                if (!connectNeeded && !close_ && commands_.empty()) changed_.wait_for(lock, std::chrono::milliseconds(200), [&] { return close_ || !commands_.empty(); });
                closing = close_; shuttingDown = shutdown_;
                if (!closing && !commands_.empty()) { command = commands_.front(); commands_.pop_front(); hasCommand = true; activating_=command.action==Action::Activate?command.window:0; }
            }
            if (closing && !possibleLease && !client.registered()) { finished(); break; }
            if (connectNeeded || (hasCommand && command.action == Action::Initialize && (!client.registered() || !snapshot().connected))) {
                if (!closing || possibleLease) {
                    client = RecoveryClient{};
                    initialized = false;
                    const DWORD error = client.connect(module_, windows_);
                    possibleLease = possibleLease || client.registered();
                    { std::scoped_lock lock(mutex_); status_.connected = !error; }
                    publish(error ? failed(error, client.lastReply()) : client.lastReply());
                    connectNeeded = false;
                    if (!error && !closing) { publish(client.initialize()); initialized = true; }
                }
            }
            // A close arriving during payload extraction/registration must also
            // release that newly created lease, even if no frame was hidden.
            { std::scoped_lock lock(mutex_); closing = close_; shuttingDown = shutdown_; }
            if (closing) {
                if (!possibleLease && !client.registered()) { finished(); break; }
                auto reply = client.release();
                if (!reply.error && !reply.remaining) { possibleLease = false; finished(); break; }
                const bool accepted = reply.state == ipc::State::Restoring || reply.state == ipc::State::RestoreFailed;
                if (reply.error == ERROR_IO_PENDING || (reply.error == ERROR_BUSY && reply.state == ipc::State::Restoring)) reply.state = ipc::State::Restoring;
                else reply.state = ipc::State::RestoreFailed;
                if (!reply.message[0]) wcscpy_s(reply.message, L"尚未确认全部恢复，记录已保留，请重试恢复");
                publish(reply);
                // Unexpected window destruction cannot keep its UI alive. Once
                // Release is acknowledged, the helper owns retry and its prompt.
                if (shuttingDown && accepted) break;
                if (transportFault(reply.error)) connectNeeded = true;
                std::unique_lock lock(mutex_); changed_.wait_for(lock, std::chrono::milliseconds(700));
                continue;
            }
            if (hasCommand && client.registered()) {
                ipc::Reply reply;
                do {
                    publish(snapshot().reply, true);
                    if (command.action == Action::Initialize) { reply = client.initialize(); initialized = true; }
                    else if (command.action == Action::Hide) reply = client.hide(command.window);
                    else reply = client.show(command.window);
                    publish(reply, reply.error == ERROR_IO_PENDING, true);
                    if (reply.error != ERROR_IO_PENDING) break;
                    std::unique_lock lock(mutex_); changed_.wait_for(lock, std::chrono::milliseconds(100));
                    if (close_) break;
                } while (true);
                if (!reply.error && command.action == Action::Activate && onActivate_) onActivate_(command.window);
                { std::scoped_lock lock(mutex_); activating_=0; }
            } else if (hasCommand && command.action == Action::Activate && !possibleLease && onActivate_) onActivate_(command.window);
            if (client.registered()) {
                const auto reply = client.query();
                { std::scoped_lock lock(mutex_); if (!reply.error) status_.connected = true; else if (transportFault(reply.error)) status_.connected = false; }
                publish(reply);
                if (!reply.error && !initialized) { publish(client.initialize()); initialized = true; }
            }
        }
    } catch (...) {
        // Do not report a successful close merely because the worker failed.
        // Recovery remains serialized and the canvas stays until acknowledged.
        { std::scoped_lock lock(mutex_); close_ = status_.closing = true; }
        for (;;) {
            if (!possibleLease && !client.registered()) { finished(); break; }
            auto reply = client.release();
            if (!reply.error && !reply.remaining) { finished(); break; }
            const bool accepted = reply.state == ipc::State::Restoring || reply.state == ipc::State::RestoreFailed;
            reply.state = ipc::State::RestoreFailed;
            try { publish(reply); } catch (...) {}
            { std::scoped_lock lock(mutex_); if (shutdown_ && accepted) break; }
            if (transportFault(reply.error)) {
                client = RecoveryClient{};
                try { client.connect(module_, windows_); } catch (...) {}
            }
            Sleep(700);
        }
    }
    if (previous) SetThreadDpiAwarenessContext(previous);
}
}
