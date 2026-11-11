#pragma once

// "Games" window, opened from the status window: every game the catalog found, with a checkbox
// to exclude it, plus folders the user adds for games no catalog lists. Changes go back to the
// tray through callbacks; the tray saves them and rebuilds the matcher.

#include "core/games.h"
#include "tray/game_settings.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace myalien::tray {

class GamesDialog {
public:
    struct Callbacks {
        std::function<void(const std::wstring& key, bool included)> setIncluded;
        std::function<void(const std::wstring& folder)> addFolder;
        std::function<void(const std::wstring& folder)> removeFolder;
        std::function<void()> rescan;
    };

    GamesDialog(HINSTANCE instance, Callbacks callbacks);
    ~GamesDialog();
    GamesDialog(const GamesDialog&) = delete;
    GamesDialog& operator=(const GamesDialog&) = delete;

    void show(HWND owner);
    bool visible() const;
    HWND hwnd() const noexcept { return hwnd_; }  // for IsDialogMessage

    // cat null: the list has not been read yet.
    void update(const games::Catalog* cat, const GameSettings& settings);

private:
    struct Row {
        games::Game game;
        bool included = true;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create(HWND owner);
    void layout();
    void applyFont();
    void fill();         // rows_ -> list view, honouring the filter
    void updateButtons();
    void addFolder();
    int scale(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    HWND item(int id) const { return GetDlgItem(hwnd_, id); }
    const Row* selectedRow() const;

    HINSTANCE instance_;
    Callbacks callbacks_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    bool filling_ = false;

    std::vector<Row> rows_;
    std::wstring footer_;
};

}  // namespace myalien::tray
