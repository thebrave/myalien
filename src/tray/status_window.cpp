#include "tray/status_window.h"

#include "core/awcc.h"
#include "core/error.h"
#include "tray/icon.h"
#include "tray/theme.h"
#include "tray/about.h"
#include "tray/requests.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace myalien::tray {

namespace {

constexpr wchar_t kClassName[] = L"MyAlienStatusWindow";
constexpr int kClientW = 420;
constexpr int kClientH = 634;
constexpr DWORD kChargeTimeoutMs = 40000;

enum Id : int {
    IDC_GRP_THERMAL = 100,
    IDC_LBL_PROFILE,
    IDC_PROFILE,
    IDC_GMODE,
    IDC_LBL_FAN0,
    IDC_FAN0,
    IDC_LBL_FAN1,
    IDC_FAN1,
    IDC_NOTE,
    IDC_LBL_BOOST0,
    IDC_BOOST0,
    IDC_BOOSTV0,
    IDC_LBL_BOOST1,
    IDC_BOOST1,
    IDC_BOOSTV1,
    IDC_APPLY_BOOST,
    IDC_GRP_BATTERY,
    IDC_BAT_MODEL,
    IDC_BAT_STATE,
    IDC_LBL_CHARGE,
    IDC_CHARGE,
    IDC_LBL_RANGE,
    IDC_START,
    IDC_START_UD,
    IDC_DASH,
    IDC_STOP,
    IDC_STOP_UD,
    IDC_PCT,
    IDC_APPLY_RANGE,
    IDC_CHARGE_AGE,
    IDC_GRP_GAMES,
    IDC_GM_ENABLE,
    IDC_GM_LBL_PROFILE,
    IDC_GM_PROFILE,
    IDC_GM_LBL_POWER,
    IDC_GM_POWER,
    IDC_GM_AC,
    IDC_GM_NOW,
    IDC_GM_GAMES,
    IDC_STATUS,
    IDC_LINK,
};

bool isGameControl(int id) { return id >= IDC_GRP_GAMES && id <= IDC_GM_GAMES; }

// Power mode targets in the Games group, in combo order.
constexpr std::pair<std::optional<myalien::PowerMode>, const wchar_t*> kGamePowerItems[] = {
    {std::nullopt, L"Leave unchanged"},
    {myalien::PowerMode::BestEfficiency, L"Best power efficiency"},
    {myalien::PowerMode::Balanced, L"Balanced"},
    {myalien::PowerMode::BestPerformance, L"Best performance"},
};

struct ControlDef {
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    int x, y, w, h;  // 96-DPI client coordinates
};

constexpr DWORD kStatic = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
constexpr DWORD kStaticEllipsis = kStatic | SS_ENDELLIPSIS;
constexpr DWORD kCombo = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST;
constexpr DWORD kButton = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
constexpr DWORD kTrack = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS;
constexpr DWORD kEdit = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT;
constexpr DWORD kUpDown = WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS;

// Combo heights include the drop-down list.
constexpr ControlDef kControls[] = {
    {IDC_GRP_THERMAL, WC_BUTTONW, L"Thermal", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 8, 400, 226},
    {IDC_LBL_PROFILE, WC_STATICW, L"Profile", kStatic, 24, 35, 80, 20},
    {IDC_PROFILE, WC_COMBOBOXW, L"", kCombo, 110, 31, 180, 220},
    {IDC_GMODE, WC_BUTTONW, L"G-Mode", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX, 300, 32, 104, 22},
    {IDC_LBL_FAN0, WC_STATICW, L"", kStatic, 24, 66, 80, 20},
    {IDC_FAN0, WC_STATICW, L"", kStaticEllipsis, 110, 66, 290, 20},
    {IDC_LBL_FAN1, WC_STATICW, L"", kStatic, 24, 90, 80, 20},
    {IDC_FAN1, WC_STATICW, L"", kStaticEllipsis, 110, 90, 290, 20},
    {IDC_NOTE, WC_STATICW, L"", kStaticEllipsis, 24, 116, 376, 20},
    {IDC_LBL_BOOST0, WC_STATICW, L"", kStatic, 24, 146, 80, 20},
    {IDC_BOOST0, TRACKBAR_CLASSW, L"", kTrack, 104, 142, 244, 28},
    {IDC_BOOSTV0, WC_STATICW, L"", kStatic, 352, 146, 48, 20},
    {IDC_LBL_BOOST1, WC_STATICW, L"", kStatic, 24, 176, 80, 20},
    {IDC_BOOST1, TRACKBAR_CLASSW, L"", kTrack, 104, 172, 244, 28},
    {IDC_BOOSTV1, WC_STATICW, L"", kStatic, 352, 176, 48, 20},
    {IDC_APPLY_BOOST, WC_BUTTONW, L"Apply boost", kButton, 290, 202, 110, 26},
    {IDC_GRP_BATTERY, WC_BUTTONW, L"Battery", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 244, 400, 196},
    {IDC_BAT_MODEL, WC_STATICW, L"", kStaticEllipsis, 24, 268, 376, 20},
    {IDC_BAT_STATE, WC_STATICW, L"", kStaticEllipsis, 24, 292, 376, 20},
    {IDC_LBL_CHARGE, WC_STATICW, L"Charging", kStatic, 24, 326, 80, 20},
    {IDC_CHARGE, WC_COMBOBOXW, L"", kCombo, 110, 322, 180, 220},
    {IDC_LBL_RANGE, WC_STATICW, L"Custom range", kStatic, 24, 362, 84, 20},
    {IDC_START, WC_EDITW, L"", kEdit, 110, 358, 54, 24},
    {IDC_START_UD, UPDOWN_CLASSW, L"", kUpDown, 0, 0, 0, 0},
    {IDC_DASH, WC_STATICW, L"–", kStatic | SS_CENTER, 166, 362, 16, 20},
    {IDC_STOP, WC_EDITW, L"", kEdit, 184, 358, 54, 24},
    {IDC_STOP_UD, UPDOWN_CLASSW, L"", kUpDown, 0, 0, 0, 0},
    {IDC_PCT, WC_STATICW, L"%", kStatic, 242, 362, 20, 20},
    {IDC_APPLY_RANGE, WC_BUTTONW, L"Use range", kButton, 290, 356, 110, 26},
    {IDC_CHARGE_AGE, WC_STATICW, L"", kStaticEllipsis, 24, 400, 376, 20},
    {IDC_GRP_GAMES, WC_BUTTONW, L"Games", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 450, 400, 152},
    {IDC_GM_ENABLE, WC_BUTTONW, L"Game mode: while a game is in front, switch to", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 24, 474, 376, 22},
    {IDC_GM_LBL_PROFILE, WC_STATICW, L"Thermal profile", kStatic, 42, 506, 96, 20},
    {IDC_GM_PROFILE, WC_COMBOBOXW, L"", kCombo, 140, 502, 160, 220},
    {IDC_GM_LBL_POWER, WC_STATICW, L"Power mode", kStatic, 42, 538, 96, 20},
    {IDC_GM_POWER, WC_COMBOBOXW, L"", kCombo, 140, 534, 160, 220},
    {IDC_GM_AC, WC_BUTTONW, L"Only on AC", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 310, 504, 94, 22},
    {IDC_GM_GAMES, WC_BUTTONW, L"Games…", kButton, 310, 532, 90, 26},
    {IDC_GM_NOW, WC_STATICW, L"", kStaticEllipsis, 24, 572, 376, 20},
    {IDC_STATUS, WC_STATICW, L"", kStaticEllipsis, 12, 608, 300, 20},
    {IDC_LINK, WC_LINK, L"<a href=\"" MYALIEN_PROJECT_URL L"\">Project page</a>",
     WS_CHILD | WS_VISIBLE | WS_TABSTOP | LWS_TRANSPARENT | LWS_RIGHT, 316, 608, 92, 20},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::wstring opt(const std::optional<uint32_t>& v) { return v ? std::to_wstring(*v) : L"n/a"; }

}  // namespace

// ---------------------------------------------------------------------------------------------
StatusWindow::StatusWindow(HINSTANCE instance, WriteFn write) : instance_(instance), write_(std::move(write)) {}

StatusWindow::~StatusWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
}

bool StatusWindow::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

int StatusWindow::scale(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }

void StatusWindow::show() {
    if (!hwnd_) create();
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void StatusWindow::create() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = appIcon(instance_, false);
    wc.hIconSm = appIcon(instance_, true);
    RegisterClassExW(&wc);  // fails harmlessly if already registered

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;  // close only: no greyed maximize box
    hwnd_ = CreateWindowExW(0, kClassName, L"MyAlien", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                            instance_, this);
    if (!hwnd_) return;
    dpi_ = GetDpiForWindow(hwnd_);

    for (const auto& c : kControls) {
        CreateWindowExW(wcscmp(c.cls, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0, c.cls, c.text, c.style, 0, 0, 0, 0, hwnd_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), instance_, nullptr);
    }
    for (int id : {IDC_BOOST0, IDC_BOOST1}) {
        SendMessageW(item(id), TBM_SETRANGE, FALSE, MAKELPARAM(0, kMaxFanBoost));
        SendMessageW(item(id), TBM_SETTICFREQ, 10, 0);
        SendMessageW(item(id), TBM_SETPAGESIZE, 0, 10);
    }
    for (const auto& [mode, label] : kGamePowerItems)
        SendMessageW(item(IDC_GM_POWER), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(item(IDC_START_UD), UDM_SETRANGE32, 50, 95);
    SendMessageW(item(IDC_STOP_UD), UDM_SETRANGE32, 55, 100);
    SetWindowTextW(item(IDC_STATUS), L"MyAlien " MYALIEN_VERSION);  // until the first change
    theme::apply(hwnd_);

    applyFont();
    layout();
    refreshControls();
}

void StatusWindow::applyFont() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    HFONT fresh = CreateFontIndirectW(&ncm.lfMessageFont);
    for (const auto& c : kControls) SendMessageW(item(c.id), WM_SETFONT, reinterpret_cast<WPARAM>(fresh), FALSE);
    if (font_) DeleteObject(font_);
    font_ = fresh;
}

void StatusWindow::layout() {
    RECT r{0, 0, scale(kClientW), scale(kClientH)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    for (const auto& c : kControls) {
        if (c.w == 0) continue;  // up-down controls follow their buddy
        MoveWindow(item(c.id), scale(c.x), scale(c.y), scale(c.w), scale(c.h), FALSE);
    }
    // Re-attaching the buddy re-positions the up-down against the (moved) edit.
    SendMessageW(item(IDC_START_UD), UDM_SETBUDDY, reinterpret_cast<WPARAM>(item(IDC_START)), 0);
    SendMessageW(item(IDC_STOP_UD), UDM_SETBUDDY, reinterpret_cast<WPARAM>(item(IDC_STOP)), 0);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------------------------
void StatusWindow::update(const Snapshot* s, const std::string& error, bool busy, uint8_t lastRegular) {
    if (s) snapshot_ = *s;
    else snapshot_.reset();
    error_ = error;
    busy_ = busy;
    lastRegular_ = lastRegular;
    if (hwnd_) refreshControls();
}

void StatusWindow::setStatusText(const std::wstring& text) {
    if (hwnd_) SetWindowTextW(item(IDC_STATUS), text.c_str());
}

void StatusWindow::refreshControls() {
    updating_ = true;
    const bool connected = snapshot_ && error_.empty();
    const bool inputs = connected && !busy_;

    if (!connected) {
        for (const auto& c : kControls)
            if (!isGameControl(c.id) && c.id != IDC_LINK) EnableWindow(item(c.id), FALSE);
        SetWindowTextW(item(IDC_STATUS), (L"MyAlien service: " + toWide(error_.empty() ? std::string("connecting…") : error_)).c_str());
        updating_ = false;
        refreshGames();
        return;
    }
    for (const auto& c : kControls) EnableWindow(item(c.id), TRUE);
    const Snapshot& s = *snapshot_;

    // --- thermal
    const bool thermal = !s.thermalError;
    const bool stuck = thermal && gameShiftStuck(s.profile, s.gameShift);
    const bool gmode = thermal && s.profile == profile::GMode;

    // Firmware profiles, Custom, and G-Mode (shown "G-Mode (stuck)" while Game Shift is stuck).
    std::vector<uint8_t> profiles = s.profiles;
    profiles.push_back(profile::Custom);
    if (s.gameShiftSupported) profiles.push_back(profile::GMode);
    HWND combo = item(IDC_PROFILE);
    if (profiles != profileItems_ || stuck != profileListStuck_) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (uint8_t p : profiles) {
            const std::wstring label = p == profile::GMode && stuck ? L"G-Mode (stuck)" : toWide(profileName(p));
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        profileItems_ = profiles;
        profileListStuck_ = stuck;
    }
    const uint8_t shown = stuck ? profile::GMode : s.profile;  // fans behave as in G-Mode
    int sel = -1;
    if (thermal)
        for (size_t i = 0; i < profileItems_.size(); ++i)
            if (profileItems_[i] == shown) sel = static_cast<int>(i);
    if (SendMessageW(combo, CB_GETCURSEL, 0, 0) != sel && !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0))
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);

    SetWindowTextW(item(IDC_GMODE), stuck ? L"G-Mode (stuck)" : L"G-Mode");
    SendMessageW(item(IDC_GMODE), BM_SETCHECK, (gmode || stuck) ? BST_CHECKED : BST_UNCHECKED, 0);

    for (int i = 0; i < 2; ++i) {
        const int lbl = i == 0 ? IDC_LBL_FAN0 : IDC_LBL_FAN1;
        const int val = i == 0 ? IDC_FAN0 : IDC_FAN1;
        const int blbl = i == 0 ? IDC_LBL_BOOST0 : IDC_LBL_BOOST1;
        const int bar = i == 0 ? IDC_BOOST0 : IDC_BOOST1;
        const int bval = i == 0 ? IDC_BOOSTV0 : IDC_BOOSTV1;
        if (!thermal || i >= static_cast<int>(s.fans.size())) {
            for (int id : {lbl, val, blbl, bar, bval}) ShowWindow(item(id), SW_HIDE);
            continue;
        }
        for (int id : {lbl, val, blbl, bar, bval}) ShowWindow(item(id), SW_SHOW);
        const auto& f = s.fans[static_cast<size_t>(i)];
        const std::wstring name = toWide(fanName(f.id));
        SetWindowTextW(item(lbl), name.c_str());
        SetWindowTextW(item(blbl), (name + L" boost").c_str());
        const std::wstring line = std::format(L"{} · {} rpm (max {}) · boost {}",
                                              f.tempC ? std::format(L"{} °C", *f.tempC) : std::wstring(L"n/a"),
                                              opt(f.rpm), opt(f.maxRpm), opt(f.boost));
        SetWindowTextW(item(val), line.c_str());
        if (!boostDirty_) SendMessageW(item(bar), TBM_SETPOS, TRUE, static_cast<LPARAM>(f.boost.value_or(0)));
        SetWindowTextW(item(bval), std::format(L"{} %", SendMessageW(item(bar), TBM_GETPOS, 0, 0)).c_str());
    }

    const bool custom = thermal && s.profile == profile::Custom;
    std::wstring note;
    if (!thermal) note = L"Thermal data unavailable: " + toWide(s.thermalError.message);
    else if (stuck) note = L"Game Shift is stuck on: fans at full boost. Untick G-Mode to fix it.";
    else if (gmode) note = L"G-Mode: the firmware runs both fans at full boost.";
    else if (!custom) note = L"Fan boost can be changed in the Custom profile.";
    SetWindowTextW(item(IDC_NOTE), note.c_str());

    EnableWindow(combo, inputs && thermal);
    EnableWindow(item(IDC_GMODE), inputs && thermal && s.gameShiftSupported);
    for (int id : {IDC_BOOST0, IDC_BOOST1, IDC_APPLY_BOOST}) EnableWindow(item(id), inputs && custom);
    if (!custom) boostDirty_ = false;

    // --- battery
    if (!s.batteries.empty()) {
        const auto& b = s.batteries.front();
        SetWindowTextW(item(IDC_BAT_MODEL), toWide(std::format("{} ({}, {})", b.model, b.manufacturer, b.chemistry)).c_str());
        std::wstring st;
        if (const auto c = b.chargePercent()) st += std::format(L"{:.0f} %", *c);
        st += b.charging ? L", charging" : b.discharging ? L", discharging" : (b.onLine ? L", on AC" : L"");
        if (const auto h = b.healthPercent()) st += std::format(L" · health {:.1f} % ({:.1f} of {:.1f} Wh)", *h,
                                                                b.fullChargeCapacity / 1000.0, b.designCapacity / 1000.0);
        SetWindowTextW(item(IDC_BAT_STATE), st.c_str());
    } else {
        SetWindowTextW(item(IDC_BAT_MODEL), s.batteryError ? toWide(s.batteryError.message).c_str() : L"No battery found");
        SetWindowTextW(item(IDC_BAT_STATE), L"");
    }

    const bool charge = s.chargeExposed && !s.chargeError;
    HWND ccombo = item(IDC_CHARGE);
    if (s.chargeModes != chargeItems_) {
        SendMessageW(ccombo, CB_RESETCONTENT, 0, 0);
        for (const auto& m : s.chargeModes) SendMessageW(ccombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(chargeModeLabel(m).c_str()));
        chargeItems_ = s.chargeModes;
    }
    int csel = -1;
    for (size_t i = 0; i < chargeItems_.size(); ++i)
        if (chargeItems_[i] == s.chargeMode) csel = static_cast<int>(i);
    if (SendMessageW(ccombo, CB_GETCURSEL, 0, 0) != csel && !SendMessageW(ccombo, CB_GETDROPPEDSTATE, 0, 0))
        SendMessageW(ccombo, CB_SETCURSEL, static_cast<WPARAM>(csel), 0);

    if (!rangeDirty_ && s.chargeStart && s.chargeStop) {
        SetDlgItemInt(hwnd_, IDC_START, static_cast<UINT>(*s.chargeStart), FALSE);
        SetDlgItemInt(hwnd_, IDC_STOP, static_cast<UINT>(*s.chargeStop), FALSE);
    }
    for (int id : {IDC_CHARGE, IDC_START, IDC_START_UD, IDC_STOP, IDC_STOP_UD, IDC_APPLY_RANGE})
        EnableWindow(item(id), inputs && charge);

    std::wstring age;
    if (s.chargeError) age = L"Charge settings: " + toWide(s.chargeError.message);
    else if (s.chargeAgeSeconds) {
        const uint32_t a = *s.chargeAgeSeconds;
        age = a < 60 ? std::format(L"Charge settings read {} s ago", a) : std::format(L"Charge settings read {} min ago", a / 60);
        if (s.chargeMode != "Custom") age += L" · the range applies in Custom";
    }
    SetWindowTextW(item(IDC_CHARGE_AGE), age.c_str());

    if (busy_) SetWindowTextW(item(IDC_STATUS), L"Applying…");
    updating_ = false;
    refreshGames();
}

void StatusWindow::updateGames(const GameView& v) {
    games_ = v;
    if (hwnd_) refreshGames();
}

void StatusWindow::refreshGames() {
    updating_ = true;
    const GameSettings& g = games_.settings;
    for (int id = IDC_GRP_GAMES; id <= IDC_GM_GAMES; ++id) EnableWindow(item(id), TRUE);
    SendMessageW(item(IDC_GM_ENABLE), BM_SETCHECK, g.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(item(IDC_GM_AC), BM_SETCHECK, g.onlyOnAc ? BST_CHECKED : BST_UNCHECKED, 0);

    // Thermal targets: leave unchanged, the firmware's profiles, G-Mode; plus the saved target
    // when the service has not told us the list yet.
    std::vector<std::optional<uint8_t>> items{std::nullopt};
    const bool thermal = snapshot_ && !snapshot_->thermalError;
    if (thermal)
        for (uint8_t p : snapshot_->profiles) items.emplace_back(p);
    if (!thermal || snapshot_->gameShiftSupported) items.emplace_back(profile::GMode);
    if (g.targets.profile && std::find(items.begin(), items.end(), g.targets.profile) == items.end())
        items.push_back(g.targets.profile);
    HWND pc = item(IDC_GM_PROFILE);
    if (items != gameProfileItems_) {
        SendMessageW(pc, CB_RESETCONTENT, 0, 0);
        for (const auto& p : items) {
            const std::wstring label = p ? toWide(profileName(*p)) : std::wstring(L"Leave unchanged");
            SendMessageW(pc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        gameProfileItems_ = items;
    }
    for (size_t i = 0; i < gameProfileItems_.size(); ++i)
        if (gameProfileItems_[i] == g.targets.profile && !SendMessageW(pc, CB_GETDROPPEDSTATE, 0, 0))
            SendMessageW(pc, CB_SETCURSEL, i, 0);
    for (size_t i = 0; i < std::size(kGamePowerItems); ++i)
        if (kGamePowerItems[i].first == g.targets.power && !SendMessageW(item(IDC_GM_POWER), CB_GETDROPPEDSTATE, 0, 0))
            SendMessageW(item(IDC_GM_POWER), CB_SETCURSEL, i, 0);

    SetWindowTextW(item(IDC_GM_GAMES), games_.games < 0 ? L"Games…" : std::format(L"Games ({})…", games_.games).c_str());
    SetWindowTextW(item(IDC_GM_NOW), games_.now.c_str());
    updating_ = false;
}

void StatusWindow::onGameCommand(int id, int code) {
    if (id == IDC_GM_GAMES && code == BN_CLICKED) {
        if (openGames_) openGames_();
        return;
    }
    GameSettings g = games_.settings;
    if (id == IDC_GM_ENABLE && code == BN_CLICKED) {
        g.enabled = SendMessageW(item(IDC_GM_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
    } else if (id == IDC_GM_AC && code == BN_CLICKED) {
        g.onlyOnAc = SendMessageW(item(IDC_GM_AC), BM_GETCHECK, 0, 0) == BST_CHECKED;
    } else if (id == IDC_GM_PROFILE && code == CBN_SELCHANGE) {
        const auto i = SendMessageW(item(IDC_GM_PROFILE), CB_GETCURSEL, 0, 0);
        if (i < 0 || static_cast<size_t>(i) >= gameProfileItems_.size()) return;
        g.targets.profile = gameProfileItems_[static_cast<size_t>(i)];
    } else if (id == IDC_GM_POWER && code == CBN_SELCHANGE) {
        const auto i = SendMessageW(item(IDC_GM_POWER), CB_GETCURSEL, 0, 0);
        if (i < 0 || static_cast<size_t>(i) >= std::size(kGamePowerItems)) return;
        g.targets.power = kGamePowerItems[static_cast<size_t>(i)].first;
    } else {
        return;
    }
    games_.settings = g;
    if (gameChanged_) gameChanged_(g);
}

// ---------------------------------------------------------------------------------------------
void StatusWindow::onCommand(int id, int code) {
    if (updating_) return;
    if (isGameControl(id)) {
        onGameCommand(id, code);
        return;
    }
    if (!snapshot_) return;
    const Snapshot& s = *snapshot_;

    if (id == IDC_PROFILE && code == CBN_SELCHANGE) {
        const auto i = SendMessageW(item(IDC_PROFILE), CB_GETCURSEL, 0, 0);
        if (i < 0 || static_cast<size_t>(i) >= profileItems_.size()) return;
        const uint8_t p = profileItems_[static_cast<size_t>(i)];
        const bool stuck = gameShiftStuck(s.profile, s.gameShift);
        if (p == profile::GMode) {
            if (s.profile == profile::GMode || stuck) return;  // already (or effectively) in G-Mode
            write_("GMODE ON", "G-Mode on", 15000);
            return;
        }
        if (p == s.profile && !stuck) return;
        const Request r = profileRequest(s, p);
        write_(r.text, r.what, 15000);
    } else if (id == IDC_GMODE && code == BN_CLICKED) {
        const Request r = gmodeToggleRequest(s, lastRegular_);
        write_(r.text, r.what, 15000);
    } else if (id == IDC_APPLY_BOOST && code == BN_CLICKED) {
        for (size_t i = 0; i < s.fans.size() && i < 2; ++i) {
            const auto pos = SendMessageW(item(i == 0 ? IDC_BOOST0 : IDC_BOOST1), TBM_GETPOS, 0, 0);
            if (s.fans[i].boost && static_cast<LRESULT>(*s.fans[i].boost) == pos) continue;
            const std::string name = lower(fanName(s.fans[i].id));
            write_(std::format("FAN {} {}", name, pos), std::format("Fan boost: {} {} %", fanName(s.fans[i].id), pos), 15000);
        }
        boostDirty_ = false;
    } else if (id == IDC_CHARGE && code == CBN_SELCHANGE) {
        const auto i = SendMessageW(item(IDC_CHARGE), CB_GETCURSEL, 0, 0);
        if (i < 0 || static_cast<size_t>(i) >= chargeItems_.size()) return;
        const std::string& m = chargeItems_[static_cast<size_t>(i)];
        if (m == s.chargeMode) return;
        write_("CHARGE " + lower(m), "Charging mode: " + m, kChargeTimeoutMs);
    } else if (id == IDC_APPLY_RANGE && code == BN_CLICKED) {
        BOOL okStart = FALSE, okStop = FALSE;
        const UINT start = GetDlgItemInt(hwnd_, IDC_START, &okStart, FALSE);
        const UINT stop = GetDlgItemInt(hwnd_, IDC_STOP, &okStop, FALSE);
        if (!okStart || !okStop) {
            setStatusText(L"Enter whole percentages for the custom range.");
            return;
        }
        write_(std::format("CHARGE custom {} {}", start, stop), std::format("Charging mode: Custom {}-{} %", start, stop),
               kChargeTimeoutMs);
        rangeDirty_ = false;
    } else if ((id == IDC_START || id == IDC_STOP) && code == EN_CHANGE) {
        rangeDirty_ = true;
    }
}

void StatusWindow::onScroll(HWND control) {
    if (updating_) return;
    const int id = GetDlgCtrlID(control);
    if (id != IDC_BOOST0 && id != IDC_BOOST1) return;
    boostDirty_ = true;
    const auto pos = SendMessageW(control, TBM_GETPOS, 0, 0);
    SetWindowTextW(item(id == IDC_BOOST0 ? IDC_BOOSTV0 : IDC_BOOSTV1), std::format(L"{} %", pos).c_str());
}

// ---------------------------------------------------------------------------------------------
LRESULT CALLBACK StatusWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<StatusWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StatusWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            if (lp) onScroll(reinterpret_cast<HWND>(lp));
            return 0;
        case WM_NOTIFY: {  // dark check boxes, "Project page" link
            if (LRESULT r = 0; theme::customDraw(lp, r)) return r;
            const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->idFrom == IDC_LINK && (hdr->code == NM_CLICK || hdr->code == NM_RETURN))
                ShellExecuteW(hwnd_, L"open", reinterpret_cast<const NMLINK*>(lp)->item.szUrl, nullptr, nullptr,
                              SW_SHOWNORMAL);
            return 0;
        }
        case WM_ERASEBKGND:
            return theme::eraseBackground(hwnd_, reinterpret_cast<HDC>(wp)) ? 1 : 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (HBRUSH b = theme::controlColor(msg, reinterpret_cast<HDC>(wp))) return reinterpret_cast<LRESULT>(b);
            break;
        case WM_SETTINGCHANGE:
            if (theme::isThemeChange(msg, lp)) {
                theme::refresh();
                theme::apply(hwnd_);
            }
            break;
        case WM_CLOSE:  // the tray keeps running: just hide
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            applyFont();
            layout();
            return 0;
        }
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace myalien::tray
