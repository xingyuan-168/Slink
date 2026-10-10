#pragma once
#include "client.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gwm::vd {
struct CanvasStatus {
    ipc::Reply reply;
    bool connected{}, busy{}, closing{}, finished{};
    uint64_t transientUntil{};
    std::wstring transient;
    bool hidden(uint64_t window) const;
};

// The canvas never performs IPC on its message or graphics thread.
class CanvasController {
public:
    ~CanvasController();
    void start(HINSTANCE, std::vector<uint64_t>, std::function<void()> changed,
        std::function<void()> closed, std::function<void(uint64_t)> activate);
    CanvasStatus snapshot() const;
    void hide(uint64_t);
    void show(uint64_t, bool activate = false);
    void retry();
    void close();
    void shutdown();
private:
    enum class Action { Initialize, Hide, Show, Activate };
    struct Command { Action action; uint64_t window{}; };
    void enqueue(Command);
    void run() noexcept;
    void publish(const ipc::Reply&, bool busy = false, bool operation = false);
    void finished();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    CanvasStatus status_;
    std::deque<Command> commands_;
    bool close_{}, shutdown_{};
    uint64_t activating_{};
    std::thread worker_;
    HINSTANCE module_{};
    std::vector<uint64_t> windows_;
    std::function<void()> onChanged_, onClosed_;
    std::function<void(uint64_t)> onActivate_;
};
}
