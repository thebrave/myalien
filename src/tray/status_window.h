#pragma once

// "MyAlien" status window, opened from the tray icon: live thermal and battery state, plus the
// controls the menu does not have (fan boost in Custom, custom charge range).
// Plain Win32 controls, created in code, laid out in 96-DPI units scaled to the window's DPI.

#include "core/snapshot.h"
#include "tray/game_settings.h"

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

    // Games group: current settings, the "Now:" line, and the number of known games (-1 = the
    // list has not been read yet). Works without the service (the power mode needs none).
    struct GameView {
        GameSettings settings;
        std::wstring now;
        int games = -1;
    };
    void updateGames(const GameView& v);
    // Called when the user changes a game mode setting, or presses "Games…".
    void setGameCallbacks(std::function<void(const GameSettings&)> changed, std::function<void()> openGames) {
        gameChanged_ = std::move(changed);
        openGames_ = std::move(openGames);
    }

private:
    void refreshGames();
    void onGameCommand(int id, int code);

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

    GameView games_;
    std::vector<std::optional<uint8_t>> gameProfileItems_;  // combo index -> target (nullopt = unchanged)
    std::function<void(const GameSettings&)> gameChanged_;
    std::function<void()> openGames_;
};

}  // namespace myalien::tray
