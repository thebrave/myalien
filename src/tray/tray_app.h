#pragma once

// MyAlien tray icon: shows the thermal profile as a coloured letter, temperatures in the tooltip,
// and a menu to change profile, G-Mode and charge mode through the MyAlien service.
// Runs as the logged-in user, never elevated; without the service it only shows that it is missing.

#include "core/snapshot.h"
#include "tray/worker.h"

#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <optional>
#include <string>

namespace myalien::tray {

class TrayApp {
public:
    int run(HINSTANCE instance);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void addIcon();
    void updateIcon();
    void showMenu(POINT at);
    void onCommand(UINT id);
    void notify(const std::wstring& title, const std::wstring& text, bool error);
    void write(const std::string& request, const std::string& what, DWORD timeoutMs = 15000);

    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    std::unique_ptr<Worker> worker_;
    HICON icon_ = nullptr;
    std::optional<uint8_t> iconProfile_;  // profile the current icon shows
    bool iconAdded_ = false;

    std::optional<Snapshot> snapshot_;  // last good snapshot
    std::string error_;                 // last error talking to the service (empty = fine)
    uint8_t lastRegularProfile_ = 0xA0; // profile to return to when leaving G-Mode
};

}  // namespace myalien::tray
