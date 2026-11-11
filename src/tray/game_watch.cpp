#include "tray/game_watch.h"

#include "core/games.h"

#include <memory>
#include <vector>

namespace myalien::tray {

namespace {

constexpr DWORD kQuietMs = 5000;  // after a change, wait until nothing changed for this long

// Armed change notifications for one catalog. Every handle is waitable.
class Notifications {
public:
    explicit Notifications(const std::vector<games::WatchTarget>& targets) {
        for (const auto& t : targets) {
            if (handles_.size() >= MAXIMUM_WAIT_OBJECTS - 2) break;  // keep room for stop / rescan
            if (t.kind == games::WatchTarget::Kind::Directory) {
                DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
                if (t.contentWrites) filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                const HANDLE h = FindFirstChangeNotificationW(t.path.c_str(), FALSE, filter);
                if (h != INVALID_HANDLE_VALUE) handles_.push_back({h, nullptr});
            } else {
                HKEY k = nullptr;
                const HKEY root = t.root == games::RegRoot::CurrentUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
                if (RegOpenKeyExW(root, t.path.c_str(), 0, KEY_NOTIFY, &k) != ERROR_SUCCESS) continue;
                const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                // Subkeys added or removed anywhere below. Value writes are ignored: updaters and
                // Game Bar touch values all the time.
                if (ev && RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC, ev,
                                                  TRUE) == ERROR_SUCCESS) {
                    handles_.push_back({ev, k});
                } else {
                    if (ev) CloseHandle(ev);
                    RegCloseKey(k);
                }
            }
        }
    }
    ~Notifications() {
        for (const auto& [h, key] : handles_) {
            if (key) {
                RegCloseKey(key);
                CloseHandle(h);
            } else {
                FindCloseChangeNotification(h);
            }
        }
    }
    Notifications(const Notifications&) = delete;
    Notifications& operator=(const Notifications&) = delete;

    void appendHandles(std::vector<HANDLE>& out) const {
        for (const auto& [h, key] : handles_) out.push_back(h);
    }
    size_t size() const { return handles_.size(); }

private:
    std::vector<std::pair<HANDLE, HKEY>> handles_;  // HKEY set: registry event; else a change handle
};

}  // namespace

GameWatcher::GameWatcher(HWND target)
    : target_(target),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      rescan_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      thread_([this] { loop(); }) {}

GameWatcher::~GameWatcher() {
    SetEvent(stop_);
    thread_.join();
    CloseHandle(stop_);
    CloseHandle(rescan_);
}

void GameWatcher::rescan() { SetEvent(rescan_); }

void GameWatcher::loop() {
    // Low CPU and I/O priority for the whole thread: the scan must never compete with a game.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

    for (;;) {
        auto* cat = new games::Catalog(games::scan());
        // Arm before posting, so a change during the post is not lost.
        const Notifications notes(cat->watch);
        if (!PostMessageW(target_, WM_APP_GAMES, 0, reinterpret_cast<LPARAM>(cat))) delete cat;

        std::vector<HANDLE> waits{stop_, rescan_};
        notes.appendHandles(waits);
        const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) return;
        if (r != WAIT_OBJECT_0 + 1) {
            // A catalog changed: wait for the burst to end. Re-arming every handle on each event
            // would cost more than simply waiting a fixed quiet period.
            if (WaitForSingleObject(stop_, kQuietMs) == WAIT_OBJECT_0) return;
        }
    }
}

}  // namespace myalien::tray
