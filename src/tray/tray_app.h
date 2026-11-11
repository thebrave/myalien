#pragma once

// MyAlien tray icon: shows the thermal profile as a coloured letter, temperatures in the tooltip,
// and a menu to change profile, G-Mode and charge mode through the MyAlien service.
// Runs as the logged-in user, never elevated; without the service it only shows that it is missing.

#include "core/games.h"
#include "core/snapshot.h"
#include "tray/game_mode.h"
#include "tray/game_settings.h"
#include "tray/game_watch.h"
#include "tray/games_dialog.h"
#include "tray/status_window.h"
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
    void pushToWindow();
    void noticeOutsideChanges(const Snapshot& previous, const Snapshot& now);

    // Game mode (docs/game-mode.md): detection and power mode here, profile through the service.
    void startGameMode();  // catalog watcher + foreground hook
    void stopGameMode();
    void onForegroundChange();
    void evaluateGameMode();
    void carryOut(const GameStep& step);
    GameCurrent gameCurrent() const;
    void restoreGameModeOnExit();
    std::wstring gameTargetsText() const;
    void applyGameSettings(GameSettings s);  // from the menu or the status window
    void rebuildMatcher();                   // catalog + the user's folders and exclusions
    void openGamesDialog();
    std::wstring gameNowText() const;
    std::wstring gameTipLine() const;        // tooltip: the game and what it replaced, or why nothing changed
    std::wstring replacedText() const;       // "Quiet, Balanced": what game mode will restore
    void ensureCatalog();                    // the watcher also runs while game mode is off, for the Games window
    static void CALLBACK foregroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static TrayApp* instance_;  // for the WinEvent callback (one TrayApp per process)

    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<StatusWindow> window_;
    unsigned timerTicks_ = 0;
    ULONGLONG lastWriteTick_ = 0;  // when this tray last sent a change (outside-change detection)
    HICON icon_ = nullptr;
    std::optional<uint8_t> iconProfile_;  // profile the current icon shows
    bool iconAdded_ = false;

    std::optional<Snapshot> snapshot_;  // last good snapshot
    std::string error_;                 // last error talking to the service (empty = fine)
    uint8_t lastRegularProfile_ = 0xA0; // profile to return to when leaving G-Mode

    GameSettings gameSettings_;
    GameModeController gameMode_;
    std::unique_ptr<GameWatcher> gameWatcher_;
    std::optional<games::Catalog> catalog_;  // last scan, as found (the Games window lists it)
    std::optional<games::Matcher> matcher_;  // empty until the first scan arrives
    std::unique_ptr<GamesDialog> gamesDialog_;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    std::wstring foregroundImage_;
    std::optional<games::Game> focusedGame_;  // the foreground process is this game
    std::optional<games::Matcher> ignoredMatcher_;  // games unticked in the Games window
    std::wstring ignoredGame_;                // the foreground process is this unticked game
    std::wstring activeGameName_;             // game that engaged game mode (menu, tooltip)
};

}  // namespace myalien::tray
