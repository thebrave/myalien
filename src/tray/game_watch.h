#pragma once

// Keeps the game catalog current without polling: one background-priority thread scans once,
// then sleeps in WaitForMultipleObjects on change notifications for the launcher folders and
// registry keys the scan reported. A change is followed by a quiet period (installers write in
// bursts) and one rescan. Results go to the UI thread as WM_APP_GAMES (lParam: games::Catalog*,
// deleted by the receiver).

#include <windows.h>

#include <thread>

namespace myalien::tray {

inline constexpr UINT WM_APP_GAMES = WM_APP + 4;

class GameWatcher {
public:
    explicit GameWatcher(HWND target);
    ~GameWatcher();
    GameWatcher(const GameWatcher&) = delete;
    GameWatcher& operator=(const GameWatcher&) = delete;

    void rescan();  // asks for a scan now (e.g. the Games dialog's Rescan)

private:
    void loop();

    HWND target_;
    HANDLE stop_ = nullptr;   // manual-reset
    HANDLE rescan_ = nullptr; // auto-reset
    std::thread thread_;
};

}  // namespace myalien::tray
