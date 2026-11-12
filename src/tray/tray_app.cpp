#include "tray/tray_app.h"

#include "core/actions.h"
#include "core/awcc.h"
#include "core/error.h"
#include "core/ipc.h"
#include "tray/about.h"
#include "tray/autostart.h"
#include "tray/icon.h"
#include "tray/requests.h"
#include "tray/theme.h"

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace myalien::tray {

namespace {

constexpr wchar_t kClassName[] = L"MyAlienTrayWindow";
constexpr UINT WM_APP_TRAY = WM_APP + 1;  // notify-icon callback
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 2000;

// Menu command ids.
constexpr UINT ID_PROFILE_BASE = 1000;  // + profile id (0x00..0xFF)
constexpr UINT ID_GMODE = 1400;
constexpr UINT ID_CHARGE_BASE = 1500;   // + index in snapshot.chargeModes
constexpr UINT ID_AUTOSTART = 1600;
constexpr UINT ID_REFRESH = 1601;
constexpr UINT ID_OPEN = 1602;
constexpr UINT ID_GAMEMODE = 1603;
constexpr UINT ID_ABOUT = 1604;
constexpr UINT ID_EXIT = 1699;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::wstring chargeLabel(const std::string& mode, const Snapshot& s) {
    if (mode == "PrimAcUse") return L"Primarily AC";
    if (mode == "Custom" && s.chargeStart && s.chargeStop)
        return std::format(L"Custom ({}–{} %)", *s.chargeStart, *s.chargeStop);
    return toWide(mode);
}

std::wstring tempLine(const Snapshot& s) {
    std::wstring r;
    for (const auto& f : s.fans) {
        if (!r.empty()) r += L"  ";
        r += toWide(fanName(f.id));
        r += f.tempC ? std::format(L" {} °C", *f.tempC) : std::wstring(L" n/a");
        r += f.rpm ? std::format(L" · {} rpm", *f.rpm) : std::wstring();
    }
    return r;
}

}  // namespace

TrayApp* TrayApp::instance_ = nullptr;

// ---------------------------------------------------------------------------------------------
int TrayApp::run(HINSTANCE instance) {
    instance_ = this;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // A hidden top-level window (not message-only: those do not receive "TaskbarCreated").
    hwnd_ = CreateWindowExW(0, kClassName, L"MyAlien", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    theme::apply(hwnd_);  // owner of the popup menu: lets it turn dark

    worker_ = std::make_unique<Worker>(hwnd_);
    window_ = std::make_unique<StatusWindow>(
        instance, [this](const std::string& req, const std::string& what, DWORD t) { write(req, what, t); });
    window_->setGameCallbacks([this](const GameSettings& g) { applyGameSettings(g); }, [this] { openGamesDialog(); });
    addIcon();
    worker_->requestSnapshot();
    SetTimer(hwnd_, kRefreshTimer, kRefreshMs, nullptr);

    gameSettings_ = loadGameSettings();
    if (const auto pending = loadGamePending()) gameMode_.resume(*pending);  // restored once no game is in front
    if (gameSettings_.enabled) startGameMode();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window_ && window_->hwnd() && IsDialogMessageW(window_->hwnd(), &msg)) continue;  // Tab between controls
        if (gamesDialog_ && gamesDialog_->hwnd() && IsDialogMessageW(gamesDialog_->hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stopGameMode();
    gamesDialog_.reset();
    gameWatcher_.reset();
    window_.reset();
    worker_.reset();
    instance_ = nullptr;
    return 0;
}

LRESULT CALLBACK TrayApp::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_ != 0) {  // Explorer restarted: icon is gone
        iconAdded_ = false;
        addIcon();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:      // left click / Enter: status window
                case NIN_KEYSELECT:
                    window_->show();
                    pushToWindow();
                    worker_->requestSnapshot();
                    return 0;
                case WM_CONTEXTMENU:  // right click: menu
                    showMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                    return 0;
            }
            return 0;

        case WM_TIMER:
            // Every 2 s while the window is open, every 6 s otherwise.
            if (wp == kRefreshTimer && (window_->visible() || ++timerTicks_ % 3 == 0)) worker_->requestSnapshot();
            // Leave delay and power-source changes; nothing to do while no game is involved.
            if (wp == kRefreshTimer && (gameMode_.active() || focusedGame_)) evaluateGameMode();
            return 0;

        case WM_APP_GAMES: {
            std::unique_ptr<games::Catalog> cat(reinterpret_cast<games::Catalog*>(lp));
            if (!gameWatcher_) return 0;  // stopped meanwhile
            catalog_ = std::move(*cat);
            rebuildMatcher();
            if (gamesDialog_) gamesDialog_->update(&*catalog_, gameSettings_);
            return 0;
        }

        case WM_APP_SNAPSHOT: {
            std::unique_ptr<SnapshotResult> r(reinterpret_cast<SnapshotResult*>(lp));
            if (r->snapshot) {
                if (snapshot_) noticeOutsideChanges(*snapshot_, *r->snapshot);
                snapshot_ = std::move(r->snapshot);
                error_.clear();
                if (!snapshot_->thermalError && snapshot_->profile != profile::GMode &&
                    !gameShiftStuck(snapshot_->profile, snapshot_->gameShift))
                    lastRegularProfile_ = snapshot_->profile;
            } else {
                error_ = r->error;
            }
            updateIcon();
            pushToWindow();
            if (gameMode_.active() || focusedGame_) evaluateGameMode();  // e.g. a restore waiting for the service
            return 0;
        }

        case WM_APP_WRITE_DONE: {
            std::unique_ptr<WriteResult> r(reinterpret_cast<WriteResult*>(lp));
            lastWriteTick_ = GetTickCount64();
            std::string text = r->text;
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            // Errors always; successes only when the change is slow enough to be noticed (charge),
            // or quietly in the window's status line.
            if (!r->ok) notify(toWide(r->what) + L" failed", toWide(text), true);
            else if (r->what.starts_with("Charging") && !window_->visible()) notify(toWide(r->what), toWide(text), false);
            window_->setStatusText(toWide(r->what) + (r->ok ? L": done." : L": failed. ") + (r->ok ? L"" : toWide(text)));
            pushToWindow();
            worker_->requestSnapshot();
            return 0;
        }

        case WM_COMMAND:
            onCommand(LOWORD(wp));
            return 0;

        case WM_SETTINGCHANGE:  // light / dark switch: menus follow
            if (theme::isThemeChange(msg, lp)) {
                theme::refresh();
                theme::apply(hwnd_);
            }
            return 0;

        case WM_CLOSE:  // sent by the installer before it replaces or removes the tray
            restoreGameModeOnExit();
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (iconAdded_) {
                NOTIFYICONDATAW nid{sizeof(nid)};
                nid.hWnd = hwnd_;
                nid.uID = kTrayId;
                Shell_NotifyIconW(NIM_DELETE, &nid);
            }
            if (icon_) DestroyIcon(icon_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
void TrayApp::addIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    if (!icon_) icon_ = makeProfileIcon(std::nullopt, GetSystemMetrics(SM_CXSMICON));
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"MyAlien: connecting to the service…");
    iconAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (iconAdded_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        updateIcon();
    }
}

void TrayApp::updateIcon() {
    if (!iconAdded_) return;

    std::optional<uint8_t> profile;
    std::wstring tip;
    if (!snapshot_ || !error_.empty()) {
        tip = L"MyAlien: " + toWide(error_.empty() ? std::string("connecting…") : error_);
    } else if (snapshot_->thermalError) {
        tip = L"MyAlien: thermal data unavailable";
    } else {
        const bool stuck = gameShiftStuck(snapshot_->profile, snapshot_->gameShift);
        profile = stuck ? profile::GMode : snapshot_->profile;  // fans behave as in G-Mode: show the G badge
        tip = L"MyAlien · " + toWide(profileName(snapshot_->profile)) +
              (stuck ? L" · G-Mode stuck (fans at full boost)" : L"");
        if (const std::wstring game = gameTipLine(); !game.empty()) tip += L"\n" + game;
        tip += L"\n" + tempLine(*snapshot_);
    }
    if (tip.size() > 127) tip.resize(127);

    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(nid.szTip, tip.c_str());

    HICON fresh = nullptr;
    if (profile != iconProfile_) {
        fresh = makeProfileIcon(profile, GetSystemMetrics(SM_CXSMICON));
        if (fresh) {
            nid.uFlags |= NIF_ICON;
            nid.hIcon = fresh;
        }
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (fresh) {  // the shell copied it: the old one can go
        if (icon_) DestroyIcon(icon_);
        icon_ = fresh;
        iconProfile_ = profile;
    }
}

void TrayApp::notify(const std::wstring& title, const std::wstring& text, bool error) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    std::wstring body = text;
    while (!body.empty() && (body.back() == L'\n' || body.back() == L' ')) body.pop_back();
    wcsncpy_s(nid.szInfo, body.empty() ? L" " : body.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::write(const std::string& request, const std::string& what, DWORD timeoutMs) {
    lastWriteTick_ = GetTickCount64();
    worker_->requestWrite(request, what, timeoutMs);
    pushToWindow();  // greys the window's inputs while the change runs
}

void TrayApp::pushToWindow() {
    if (!window_) return;
    window_->update(snapshot_ ? &*snapshot_ : nullptr, error_, worker_->busyWriting(), lastRegularProfile_);
    window_->updateGames({gameSettings_, gameNowText(), matcher_ ? static_cast<int>(matcher_->games().size()) : -1});
}

// Changes this tray did not ask for (MyDell, firmware, another tool): tell the user, since the
// icon alone is easy to miss. Changes within 15 s of our own write are assumed to be ours.
void TrayApp::noticeOutsideChanges(const Snapshot& prev, const Snapshot& now) {
    if (prev.thermalError || now.thermalError) return;
    const bool ours = worker_->busyWriting() || GetTickCount64() - lastWriteTick_ < 15000;
    const bool wasStuck = gameShiftStuck(prev.profile, prev.gameShift);
    const bool isStuck = gameShiftStuck(now.profile, now.gameShift);
    const std::wstring by = now.fusion == ServiceState::Running ? L" (MyDell is running)" : L"";

    if (isStuck && !wasStuck) {
        notify(L"G-Mode stuck: fans at full boost",
               L"Game Shift stayed on after a profile change" + by + L". Click the icon and untick G-Mode to fix it.", true);
    } else if (!ours && now.profile != prev.profile) {
        notify(L"Thermal profile changed",
               toWide(profileName(prev.profile)) + L" → " + toWide(profileName(now.profile)) + by, false);
    }
}

// ---------------------------------------------------------------------------------------------
void TrayApp::showMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    const bool connected = snapshot_ && error_.empty();
    const bool busy = worker_->busyWriting();
    const UINT enabled = (connected && !busy) ? MF_ENABLED : MF_GRAYED;

    if (!connected) {
        const std::wstring msg = L"MyAlien service: " + toWide(error_.empty() ? std::string("connecting…") : error_);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, msg.c_str());
    } else {
        const Snapshot& s = *snapshot_;
        if (s.thermalError) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Thermal data unavailable");
        } else {
            const bool stuck = gameShiftStuck(s.profile, s.gameShift);
            std::wstring head = toWide(profileName(s.profile));
            if (stuck) head += L"  ·  Game Shift stuck on: fans at full boost";
            if (busy) head += L"  (applying…)";
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, head.c_str());
            const std::wstring temps = tempLine(s);
            if (!temps.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, temps.c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

            std::vector<uint8_t> profiles = s.profiles;
            profiles.push_back(profile::Custom);
            for (uint8_t p : profiles) {
                const UINT flags = MF_STRING | enabled | (p == s.profile ? MF_CHECKED : MF_UNCHECKED);
                AppendMenuW(menu, flags, ID_PROFILE_BASE + p, toWide(profileName(p)).c_str());
                if (p == s.profile) CheckMenuRadioItem(menu, ID_PROFILE_BASE + p, ID_PROFILE_BASE + p, ID_PROFILE_BASE + p, MF_BYCOMMAND);
            }
            if (s.gameShiftSupported)
                AppendMenuW(menu, MF_STRING | enabled | (s.profile == profile::GMode || stuck ? MF_CHECKED : MF_UNCHECKED),
                            ID_GMODE, stuck ? L"G-Mode stuck: click to turn it off" : L"G-Mode (maximum cooling)");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (const auto& b : s.batteries) {
            std::wstring line = L"Battery";
            if (const auto c = b.chargePercent()) line += std::format(L" {:.0f} %", *c);
            line += b.charging ? L", charging" : b.discharging ? L", discharging" : (b.onLine ? L", on AC" : L"");
            if (const auto h = b.healthPercent()) line += std::format(L" · health {:.0f} %", *h);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
        }
        if (s.chargeExposed && !s.chargeError) {
            HMENU charge = CreatePopupMenu();
            for (size_t i = 0; i < s.chargeModes.size(); ++i) {
                const std::string& m = s.chargeModes[i];
                const UINT id = ID_CHARGE_BASE + static_cast<UINT>(i);
                AppendMenuW(charge, MF_STRING | enabled, id, chargeLabel(m, s).c_str());
                if (m == s.chargeMode) CheckMenuRadioItem(charge, id, id, id, MF_BYCOMMAND);
            }
            const std::wstring label = L"Charging: " + chargeLabel(s.chargeMode, s);
            AppendMenuW(menu, MF_POPUP | (busy ? MF_GRAYED : MF_ENABLED), reinterpret_cast<UINT_PTR>(charge), label.c_str());
        } else if (s.chargeError) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"Charging: " + toWide(s.chargeError.message)).c_str());
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (const std::wstring line = gameTipLine(); !line.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    AppendMenuW(menu, MF_STRING | (gameSettings_.enabled ? MF_CHECKED : MF_UNCHECKED), ID_GAMEMODE,
                L"Game mode: switch automatically while a game is in front");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open MyAlien…");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);
    AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About MyAlien…");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    // Standard tray-menu dance: foreground first so the menu closes when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, at.x, at.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
}

void TrayApp::onCommand(UINT id) {
    if (id == ID_EXIT) {
        restoreGameModeOnExit();
        DestroyWindow(hwnd_);
        return;
    }
    if (id == ID_GAMEMODE) {
        GameSettings g = gameSettings_;
        g.enabled = !g.enabled;
        applyGameSettings(std::move(g));
        return;
    }
    if (id == ID_ABOUT) {
        showAbout(window_ && window_->visible() ? window_->hwnd() : nullptr,
                  reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE)));
        return;
    }
    if (id == ID_REFRESH) {
        worker_->requestSnapshot();
        return;
    }
    if (id == ID_OPEN) {
        window_->show();
        pushToWindow();
        return;
    }
    if (id == ID_AUTOSTART) {
        if (!setAutostart(!autostartEnabled()))
            notify(L"Start with Windows", L"Could not change the Run registry key.", true);
        return;
    }
    if (!snapshot_) return;
    const Snapshot& s = *snapshot_;

    const bool stuck = gameShiftStuck(s.profile, s.gameShift);
    if (id == ID_GMODE) {
        const Request r = gmodeToggleRequest(s, lastRegularProfile_);
        write(r.text, r.what);
        return;
    }
    if (id >= ID_PROFILE_BASE && id <= ID_PROFILE_BASE + 0xFF) {
        const auto p = static_cast<uint8_t>(id - ID_PROFILE_BASE);
        if (p == s.profile && !stuck) return;
        const Request r = profileRequest(s, p);
        write(r.text, r.what);
        return;
    }
    if (id >= ID_CHARGE_BASE && id < ID_CHARGE_BASE + s.chargeModes.size()) {
        const std::string& m = s.chargeModes[id - ID_CHARGE_BASE];
        if (m == s.chargeMode) return;
        // Several BIOS writes and reads (~1.5 s each).
        write("CHARGE " + lower(m), "Charging mode: " + m, 40000);
    }
}

// --- Game mode -------------------------------------------------------------------------------
void TrayApp::ensureCatalog() {
    if (!gameWatcher_) gameWatcher_ = std::make_unique<GameWatcher>(hwnd_);
}

void TrayApp::startGameMode() {
    ensureCatalog();
    if (!foregroundHook_)
        // Out-of-context: delivered through this thread's message loop, no DLL injection, and
        // nothing runs between foreground changes.
        foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundHook, 0,
                                          0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void TrayApp::stopGameMode() {
    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    foregroundHook_ = nullptr;
    focusedGame_.reset();
    ignoredGame_.clear();
    foregroundImage_.clear();
    updateIcon();
    if (gamesDialog_ && gamesDialog_->visible()) return;  // the Games window still shows the list
    gameWatcher_.reset();
    catalog_.reset();
    matcher_.reset();
    ignoredMatcher_.reset();
}

void TrayApp::rebuildMatcher() {
    if (!catalog_) return;
    matcher_.emplace(games::applyUserChoices(catalog_->games, gameSettings_.folders, gameSettings_.excluded));
    // Unticked games are still recognised, only to say why nothing switches.
    std::vector<games::Game> ignored;
    for (const auto& g : catalog_->games)
        if (std::any_of(gameSettings_.excluded.begin(), gameSettings_.excluded.end(), [&](const std::wstring& e) {
                return CompareStringOrdinal(e.c_str(), -1, games::gameKey(g).c_str(), -1, TRUE) == CSTR_EQUAL;
            }))
            ignored.push_back(g);
    ignoredMatcher_.emplace(std::move(ignored));
    foregroundImage_.clear();  // re-match the current foreground against the new list
    if (foregroundHook_) onForegroundChange();
    pushToWindow();
}

void TrayApp::applyGameSettings(GameSettings g) {
    const bool turnedOn = g.enabled && !gameSettings_.enabled;
    if (turnedOn) g.notified = false;  // tell once, the first time it acts
    const bool choicesChanged = g.folders != gameSettings_.folders || g.excluded != gameSettings_.excluded;
    gameSettings_ = std::move(g);
    if (!saveGameSettings(gameSettings_)) notify(L"Game mode", L"Could not save the setting.", true);
    if (gameSettings_.enabled) startGameMode();
    else stopGameMode();
    if (choicesChanged) rebuildMatcher();
    evaluateGameMode();  // turning it off restores right away
    pushToWindow();
}

namespace {
bool samePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}  // namespace

void TrayApp::openGamesDialog() {
    if (!gamesDialog_) {
        GamesDialog::Callbacks cb;
        cb.setIncluded = [this](const std::wstring& key, bool included) {
            GameSettings g = gameSettings_;
            std::erase_if(g.excluded, [&](const std::wstring& e) { return samePath(e, key); });
            if (!included) g.excluded.push_back(key);
            applyGameSettings(std::move(g));
        };
        cb.addFolder = [this](const std::wstring& folder) {
            GameSettings g = gameSettings_;
            if (std::none_of(g.folders.begin(), g.folders.end(), [&](const std::wstring& f) { return samePath(f, folder); }))
                g.folders.push_back(folder);
            applyGameSettings(std::move(g));
            gamesDialog_->update(catalog_ ? &*catalog_ : nullptr, gameSettings_);
        };
        cb.removeFolder = [this](const std::wstring& folder) {
            GameSettings g = gameSettings_;
            std::erase_if(g.folders, [&](const std::wstring& f) { return samePath(f, folder); });
            applyGameSettings(std::move(g));
            gamesDialog_->update(catalog_ ? &*catalog_ : nullptr, gameSettings_);
        };
        cb.rescan = [this] {
            ensureCatalog();
            gameWatcher_->rescan();
        };
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
        gamesDialog_ = std::make_unique<GamesDialog>(instance, std::move(cb));
    }
    ensureCatalog();
    gamesDialog_->update(catalog_ ? &*catalog_ : nullptr, gameSettings_);
    gamesDialog_->show(window_ ? window_->hwnd() : nullptr);
}

std::wstring TrayApp::gameNowText() const {
    if (!gameSettings_.enabled) return L"Game mode is off.";
    if (gameMode_.active() && !activeGameName_.empty()) {
        if (gameMode_.leaving()) return L"Left " + activeGameName_ + L": restoring in a few seconds.";
        const std::wstring back = replacedText();
        return L"Now: " + activeGameName_ + (back.empty() ? L" (already at these settings)" : L" · back to " + back + L" after");
    }
    if (!matcher_) return L"Reading the game list…";
    if (!ignoredGame_.empty()) return ignoredGame_ + L" is in front, but unticked in Games: no change.";
    if (focusedGame_ && gameSettings_.onlyOnAc && !onAcPower().value_or(true))
        return focusedGame_->name + L" is in front, but on battery: no change (Only on AC).";
    if (snapshot_ && snapshot_->fusion == ServiceState::Running)
        return L"No game in front. MyDell may switch the profile back.";
    return L"No game in front.";
}

std::wstring TrayApp::replacedText() const {
    if (!gameMode_.active()) return {};
    const GamePending& p = *gameMode_.pending();
    std::wstring t;
    if (p.savedProfile) t = toWide(profileName(*p.savedProfile));
    if (p.savedPower) t += (t.empty() ? L"" : L", ") + toWide(toString(*p.savedPower));
    return t;
}

// One line for the tooltip and the menu. Empty when no game is involved.
std::wstring TrayApp::gameTipLine() const {
    if (!gameSettings_.enabled) return {};
    if (gameMode_.active() && !activeGameName_.empty()) {
        const std::wstring was = replacedText();
        if (gameMode_.leaving()) return L"Left " + activeGameName_ + L": restoring" + (was.empty() ? L"" : L" " + was);
        return L"Game: " + activeGameName_ + (was.empty() ? L" (no change needed)" : L" · was " + was);
    }
    if (!ignoredGame_.empty()) return L"Ignored game: " + ignoredGame_ + L" (unticked in Games)";
    if (focusedGame_ && gameSettings_.onlyOnAc && !onAcPower().value_or(true))
        return L"Game: " + focusedGame_->name + L" · on battery, no change";
    return {};
}

void CALLBACK TrayApp::foregroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (instance_) instance_->onForegroundChange();
}

void TrayApp::onForegroundChange() {
    const std::wstring image = games::foregroundImage();
    if (image == foregroundImage_ && !image.empty()) return;
    foregroundImage_ = image;
    const games::Game* g = matcher_ ? matcher_->match(image) : nullptr;
    if (g) focusedGame_ = *g;
    else focusedGame_.reset();
    const games::Game* ig = !g && ignoredMatcher_ ? ignoredMatcher_->match(image) : nullptr;
    ignoredGame_ = ig ? ig->name : std::wstring();
    evaluateGameMode();
    updateIcon();  // tooltip game line (cheap: the icon itself is only redrawn when the profile changes)
    if (window_ && window_->visible()) pushToWindow();  // the Games group's "Now" line
}

GameCurrent TrayApp::gameCurrent() const {
    GameCurrent c;
    c.power = windowsPowerMode();
    if (snapshot_ && error_.empty() && !snapshot_->thermalError) c.profile = snapshot_->profile;
    return c;
}

void TrayApp::evaluateGameMode() {
    if (!snapshot_ && error_.empty()) return;  // first status answer not in yet
    const GameCurrent now = gameCurrent();
    const bool allowed = gameSettings_.enabled && focusedGame_ &&
                         (!gameSettings_.onlyOnAc || onAcPower().value_or(true));
    // A profile to restore needs the service; keep the record until it answers.
    if (!allowed && gameMode_.active() && gameMode_.pending()->appliedProfile && !now.profile) return;

    const bool wasLeaving = gameMode_.leaving();
    const GameStep step = gameSettings_.enabled
                              ? gameMode_.update(allowed, now, gameSettings_.targets, GetTickCount64())
                              : gameMode_.restoreNow(now);
    if (allowed) activeGameName_ = focusedGame_->name;  // also when switching between two games
    if (gameMode_.leaving() && !wasLeaving) {
        worker_->requestSnapshot();  // fresh profile for the restore check
        updateIcon();                // tooltip: "Left …: restoring …"
    }
    carryOut(step);
}

void TrayApp::carryOut(const GameStep& step) {
    if (step.kind == GameStep::Kind::None) return;
    // The record goes first, so a crash right after a change still restores it.
    if (step.savePending) saveGamePending(*gameMode_.pending());
    if (step.clearPending) clearGamePending();

    const std::wstring who = activeGameName_.empty() ? L"Game mode" : L"Game mode · " + activeGameName_;
    if (step.setPower) {
        if (const unsigned long rc = setWindowsPowerMode(*step.setPower); rc != 0)
            notify(who, L"Could not set the Windows power mode: " + toWide(hrMessage(HRESULT_FROM_WIN32(rc))), true);
    }
    if (step.setProfile && snapshot_) {
        const Request r = switchRequest(*snapshot_, *step.setProfile);
        write(r.text, std::string("Game mode: ") + profileName(*step.setProfile));
    }
    if (step.kind == GameStep::Kind::Enter && (step.setPower || step.setProfile) && !gameSettings_.notified) {
        notify(who, gameTargetsText() + L" while the game is in front; restored when you leave it.", false);
        gameSettings_.notified = true;
        saveGameSettings(gameSettings_);
    }
    if (step.kind == GameStep::Kind::Restore) activeGameName_.clear();
    updateIcon();
    pushToWindow();
}

// Exit from the menu: undo game mode now rather than at the next start. The profile request is
// sent synchronously (the worker is about to stop); if it fails, the record keeps that part.
void TrayApp::restoreGameModeOnExit() {
    if (!gameMode_.active()) return;
    // The last snapshot can predate game mode's own switch (Exit right after entering a game):
    // read the profile again before deciding whether it is still the one game mode set.
    if (const auto r = ipc::call("STATUS", 5000); r.response && r.response->ok) {
        try {
            snapshot_ = decodeSnapshot(r.response->body);
            error_.clear();
        } catch (const std::exception&) {
        }
    }
    const GameCurrent now = gameCurrent();
    const GamePending before = *gameMode_.pending();
    const GameStep step = gameMode_.restoreNow(now);
    if (step.setPower) setWindowsPowerMode(*step.setPower);
    // Profile unknown (service unreachable): keep that part for the next start.
    bool profileDone = !step.setProfile && (!before.appliedProfile || now.profile);
    if (step.setProfile && snapshot_) {
        const auto r = ipc::call(switchRequest(*snapshot_, *step.setProfile).text, 15000);
        profileDone = r.response && r.response->ok;
    }
    if (profileDone) {
        clearGamePending();
    } else {
        GamePending left;
        left.savedProfile = before.savedProfile;
        left.appliedProfile = before.appliedProfile;
        saveGamePending(left);
    }
}

std::wstring TrayApp::gameTargetsText() const {
    std::wstring t;
    if (gameSettings_.targets.profile) t = toWide(profileName(*gameSettings_.targets.profile));
    if (gameSettings_.targets.power) {
        if (!t.empty()) t += L", ";
        t += toWide(toString(*gameSettings_.targets.power));
    }
    return t.empty() ? L"no change" : t;
}

}  // namespace myalien::tray
