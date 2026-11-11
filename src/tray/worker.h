#pragma once

// Background thread for everything that talks to the service (STATUS polls, writes), so the tray
// window never blocks. Jobs run one at a time in submission order; results come back to the UI
// thread as window messages carrying a heap-allocated result (the receiver deletes it).

#include "core/snapshot.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace myalien::tray {

inline constexpr UINT WM_APP_SNAPSHOT = WM_APP + 2;    // lParam: SnapshotResult*
inline constexpr UINT WM_APP_WRITE_DONE = WM_APP + 3;  // lParam: WriteResult*

struct SnapshotResult {
    std::optional<Snapshot> snapshot;
    std::string error;  // when no snapshot
};

struct WriteResult {
    std::string what;  // user-facing description, e.g. "Thermal profile: Quiet"
    bool ok = false;
    std::string text;  // output lines (errors first) for a notification
};

class Worker {
public:
    explicit Worker(HWND target);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void requestSnapshot();  // coalesced: at most one pending
    void requestWrite(std::string request, std::string what, DWORD timeoutMs);
    bool busyWriting() const;

private:
    void loop();
    void post(std::function<void()> job);

    HWND target_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool snapshotPending_ = false;
    int writesPending_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace myalien::tray
