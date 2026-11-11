#pragma once

// "MyAlien" status window, opened from the tray icon: live thermal and battery state, plus the
// controls the menu does not have (fan boost in Custom, custom charge range).
// Plain Win32 controls, created in code, laid out in 96-DPI units scaled to the window's DPI.

#include "core/snapshot.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace myalien::tray {

class StatusWindow {
public:
    // Sends a pipe request through the tray's worker.
    using WriteFn = std::function<void(const std::string& request, const std::string& what, DWORD timeoutMs)>;

    StatusWindow(HINSTANCE instance, WriteFn write);
    ~StatusWindow();
    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    void show();  // creates the window on first use, then shows it in front
    bool visible() const;
    HWND hwnd() const noexcept { return hwnd_; }  // for IsDialogMessage (Tab navigation)

    // Latest state from the service (snapshot null = not connected, with `error`).
    void update(const Snapshot* s, const std::string& error, bool busy, uint8_t lastRegular);
    void setStatusText(const std::wstring& text);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void create();
    void layout();
    void applyFont();
    void refreshControls();
    void onCommand(int id, int code);
    void onScroll(HWND control);
    int scale(int v) const;

    HWND item(int id) const { return GetDlgItem(hwnd_, id); }

    HINSTANCE instance_;
    WriteFn write_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;

    std::optional<Snapshot> snapshot_;
    std::string error_;
    bool busy_ = false;
    uint8_t lastRegular_ = 0xA0;

    bool updating_ = false;     // true while controls are set from a snapshot
    bool boostDirty_ = false;   // user moved a boost slider since the last Apply
    bool rangeDirty_ = false;   // user edited the custom range since the last Apply
    std::vector<uint8_t> profileItems_;      // combo index -> profile id
    bool profileListStuck_ = false;          // G-Mode item currently labelled "(stuck)"
    std::vector<std::string> chargeItems_;   // combo index -> BIOS charge mode
};

}  // namespace myalien::tray
