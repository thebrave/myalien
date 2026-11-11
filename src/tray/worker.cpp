#include "tray/worker.h"

#include "core/actions.h"
#include "core/ipc.h"

namespace myalien::tray {

Worker::Worker(HWND target) : target_(target), thread_([this] { loop(); }) {}

Worker::~Worker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Worker::post(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool Worker::busyWriting() const {
    std::lock_guard lock(mutex_);
    return writesPending_ > 0;
}

void Worker::requestSnapshot() {
    {
        std::lock_guard lock(mutex_);
        if (snapshotPending_) return;
        snapshotPending_ = true;
    }
    post([this] {
        {
            std::lock_guard lock(mutex_);
            snapshotPending_ = false;
        }
        auto* r = new SnapshotResult;
        const auto call = ipc::call("STATUS", 5000);
        if (!call.response) {
            r->error = ipc::toString(call.error);
        } else if (!call.response->ok) {
            r->error = call.response->error;
        } else {
            try {
                r->snapshot = decodeSnapshot(call.response->body);
            } catch (const std::exception& e) {
                r->error = e.what();
            }
        }
        if (!PostMessageW(target_, WM_APP_SNAPSHOT, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void Worker::requestWrite(std::string request, std::string what, DWORD timeoutMs) {
    {
        std::lock_guard lock(mutex_);
        ++writesPending_;
    }
    post([this, request = std::move(request), what = std::move(what), timeoutMs] {
        auto* r = new WriteResult;
        r->what = what;
        const auto call = ipc::call(request, timeoutMs);
        if (!call.response) {
            r->text = ipc::toString(call.error);
        } else if (!call.response->ok) {
            r->text = call.response->error;
        } else {
            try {
                const ActionOutput o = decodeOutput(call.response->body);
                r->ok = o.exitCode == exitcode::Ok;
                for (const auto& l : o.lines)  // errors first: they are what a notification must show
                    if (l.error) r->text += l.text + "\n";
                if (r->ok)
                    for (const auto& l : o.lines)
                        if (!l.error && (l.text.starts_with("After") || l.text.starts_with("Already"))) r->text += l.text + "\n";
            } catch (const std::exception& e) {
                r->text = e.what();
            }
        }
        {
            std::lock_guard lock(mutex_);
            --writesPending_;
        }
        if (!PostMessageW(target_, WM_APP_WRITE_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void Worker::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

}  // namespace myalien::tray
