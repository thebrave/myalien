#include "tray/tray_app.h"

#include "core/actions.h"
#include "core/awcc.h"
#include "core/error.h"
#include "tray/autostart.h"
#include "tray/icon.h"
#include "tray/requests.h"

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

// ---------------------------------------------------------------------------------------------
int TrayApp::run(HINSTANCE instance) {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // A hidden top-level window (not message-only: those do not receive "TaskbarCreated").
    hwnd_ = CreateWindowExW(0, kClassName, L"MyAlien", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;

    worker_ = std::make_unique<Worker>(hwnd_);
    window_ = std::make_unique<StatusWindow>(
        instance, [this](const std::string& req, const std::string& what, DWORD t) { write(req, what, t); });
    addIcon();
    worker_->requestSnapshot();
    SetTimer(hwnd_, kRefreshTimer, kRefreshMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window_ && window_->hwnd() && IsDialogMessageW(window_->hwnd(), &msg)) continue;  // Tab between controls
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    window_.reset();
    worker_.reset();
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
            return 0;

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
              (stuck ? L" · G-Mode stuck (fans at full boost)" : L"") + L"\n" + tempLine(*snapshot_);
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
    if (window_) window_->update(snapshot_ ? &*snapshot_ : nullptr, error_, worker_->busyWriting(), lastRegularProfile_);
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
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open MyAlien…");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);
    AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    // Standard tray-menu dance: foreground first so the menu closes when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, at.x, at.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
}

void TrayApp::onCommand(UINT id) {
    if (id == ID_EXIT) {
        DestroyWindow(hwnd_);
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

}  // namespace myalien::tray
