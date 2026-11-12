#include "tray/theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <string>

namespace myalien::tray::theme {

namespace {

// Windows 11 dark palette (as in Settings / Explorer).
constexpr COLORREF kDarkWindow = RGB(32, 32, 32);
constexpr COLORREF kDarkControl = RGB(45, 45, 45);
constexpr COLORREF kDarkText = RGB(240, 240, 240);
constexpr COLORREF kDarkGrayText = RGB(140, 140, 140);
constexpr COLORREF kDarkLine = RGB(75, 75, 75);

bool g_darkPainting = false;  // mirrors dark(); read by the subclass and custom draw

HBRUSH windowBrush() {
    static HBRUSH b = CreateSolidBrush(kDarkWindow);
    return b;
}
HBRUSH controlBrush() {
    static HBRUSH b = CreateSolidBrush(kDarkControl);
    return b;
}

// uxtheme exports used by Explorer, by ordinal only (Windows 10 1903+). Looked up once; any
// missing one leaves that part light.
enum class AppMode { Default = 0, AllowDark = 1 };
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using FlushMenuThemesFn = void(WINAPI*)();
using RefreshPolicyFn = void(WINAPI*)();

struct UxTheme {
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
    RefreshPolicyFn refreshPolicy = nullptr;
};

DWORD windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        if (fn(&v) == 0) return v.dwBuildNumber;
    return 0;
}

const UxTheme& ux() {
    static const UxTheme u = [] {
        UxTheme r;
        if (windowsBuild() < 18362) return r;  // the ordinals below changed meaning before 1903
        HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return r;
        r.refreshPolicy = reinterpret_cast<RefreshPolicyFn>(GetProcAddress(m, MAKEINTRESOURCEA(104)));
        r.allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(m, MAKEINTRESOURCEA(133)));
        r.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(m, MAKEINTRESOURCEA(135)));
        r.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(m, MAKEINTRESOURCEA(136)));
        return r;
    }();
    return u;
}

bool hasStyle(HWND h, LONG_PTR typeMask, std::initializer_list<LONG_PTR> types) {
    const LONG_PTR t = GetWindowLongPtrW(h, GWL_STYLE) & typeMask;
    for (LONG_PTR x : types)
        if (t == x) return true;
    return false;
}

HFONT fontOf(HWND h) { return reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)); }

std::wstring textOf(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
    return buf;
}

// Group boxes draw their caption in black under any visual style: in dark mode, paint the frame
// (a thin grey line) and the caption ourselves. Only the frame and the caption are painted, like
// the system does, so the controls inside stay visible.
LRESULT CALLBACK groupBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_PAINT && g_darkPainting) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HGDIOBJ oldFont = SelectObject(dc, fontOf(h));
        const std::wstring text = textOf(h);
        SIZE ts{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &ts);
        RECT frame{r.left, r.top + ts.cy / 2, r.right, r.bottom};
        HBRUSH line = CreateSolidBrush(kDarkLine);
        FrameRect(dc, &frame, line);
        DeleteObject(line);
        if (!text.empty()) {
            const int pad = ts.cy / 4;
            RECT tr{r.left + 2 * pad, r.top, r.left + 2 * pad + ts.cx + 2 * pad, r.top + ts.cy};
            FillRect(dc, &tr, windowBrush());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kDarkText);
            TextOutW(dc, tr.left + pad, tr.top, text.c_str(), static_cast<int>(text.size()));
        }
        SelectObject(dc, oldFont);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, groupBoxProc, id);
    return DefSubclassProc(h, msg, wp, lp);
}

void themeControl(HWND h, bool isDark) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, static_cast<int>(std::size(cls)));
    if (ux().allowDarkModeForWindow) ux().allowDarkModeForWindow(h, isDark);

    if (_wcsicmp(cls, WC_BUTTONW) == 0) {
        // Group boxes: painted by groupBoxProc in dark mode. Check boxes keep the dark visual
        // style for their glyph; their text is drawn by customDraw (the style draws it black).
        if (hasStyle(h, BS_TYPEMASK, {BS_GROUPBOX})) {
            SetWindowSubclass(h, groupBoxProc, 1, 0);  // no-op when already subclassed
            SetWindowTheme(h, nullptr, nullptr);
        } else {
            SetWindowTheme(h, isDark ? L"DarkMode_Explorer" : nullptr, nullptr);
        }
    } else if (_wcsicmp(cls, WC_COMBOBOXW) == 0 || _wcsicmp(cls, WC_EDITW) == 0) {
        SetWindowTheme(h, isDark ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (_wcsicmp(cls, UPDOWN_CLASSW) == 0 || _wcsicmp(cls, WC_SCROLLBARW) == 0) {
        SetWindowTheme(h, isDark ? L"DarkMode_Explorer" : nullptr, nullptr);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
        SetWindowTheme(h, isDark ? L"DarkMode_Explorer" : nullptr, nullptr);
        if (HWND header = ListView_GetHeader(h)) {
            if (ux().allowDarkModeForWindow) ux().allowDarkModeForWindow(header, isDark);
            SetWindowTheme(header, isDark ? L"DarkMode_ItemsView" : nullptr, nullptr);
        }
        const COLORREF bk = isDark ? kDarkWindow : GetSysColor(COLOR_WINDOW);
        ListView_SetBkColor(h, bk);
        ListView_SetTextBkColor(h, bk);
        ListView_SetTextColor(h, isDark ? kDarkText : GetSysColor(COLOR_WINDOWTEXT));
    }
    InvalidateRect(h, nullptr, TRUE);
}

}  // namespace

void initApp() {
    if (ux().setPreferredAppMode) ux().setPreferredAppMode(AppMode::AllowDark);
    refresh();
}

namespace {
bool g_dark = false;  // cached: WM_CTLCOLOR* asks on every paint

bool readDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}
}  // namespace

bool dark() { return g_dark; }

void refresh() {
    g_dark = readDark();
    g_darkPainting = g_dark;
    if (ux().refreshPolicy) ux().refreshPolicy();
    if (ux().flushMenuThemes) ux().flushMenuThemes();
}

void apply(HWND window) {
    if (!window) return;
    const bool isDark = dark();
    if (ux().allowDarkModeForWindow) ux().allowDarkModeForWindow(window, isDark);
    const BOOL useDark = isDark;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    EnumChildWindows(
        window,
        [](HWND child, LPARAM lp) -> BOOL {
            themeControl(child, lp != 0);
            return TRUE;
        },
        isDark);
    RedrawWindow(window, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

bool eraseBackground(HWND window, HDC dc) {
    RECT r;
    GetClientRect(window, &r);
    FillRect(dc, &r, dark() ? windowBrush() : GetSysColorBrush(COLOR_BTNFACE));
    return true;
}

HBRUSH controlColor(UINT msg, HDC dc) {
    if (!dark()) return nullptr;
    const bool field = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
    SetTextColor(dc, kDarkText);
    SetBkColor(dc, field ? kDarkControl : kDarkWindow);
    return field ? controlBrush() : windowBrush();
}

bool isThemeChange(UINT msg, LPARAM lp) {
    return msg == WM_SETTINGCHANGE && lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0;
}

}  // namespace myalien::tray::theme

namespace myalien::tray::theme {

// Check boxes under the dark visual style: the glyph comes from the style, the text is drawn
// here in a light colour (the style would draw it black).
bool customDraw(LPARAM lp, LRESULT& result) {
    if (!g_darkPainting) return false;
    const auto* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
    if (cd->hdr.code != NM_CUSTOMDRAW) return false;
    HWND h = cd->hdr.hwndFrom;
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, static_cast<int>(std::size(cls)));
    if (_wcsicmp(cls, WC_BUTTONW) != 0 || !hasStyle(h, BS_TYPEMASK, {BS_CHECKBOX, BS_AUTOCHECKBOX})) return false;
    if (cd->dwDrawStage != CDDS_PREPAINT) {
        result = CDRF_DODEFAULT;
        return true;
    }

    HDC dc = cd->hdc;
    RECT r = cd->rc;
    FillRect(dc, &r, windowBrush());
    const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool hot = (cd->uItemState & CDIS_HOT) != 0, pressed = (cd->uItemState & CDIS_SELECTED) != 0;
    int state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
    if (!enabled) state = checked ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED;
    else if (pressed) state = checked ? CBS_CHECKEDPRESSED : CBS_UNCHECKEDPRESSED;
    else if (hot) state = checked ? CBS_CHECKEDHOT : CBS_UNCHECKEDHOT;

    SIZE box{13, 13};
    if (HTHEME th = OpenThemeData(h, L"Button")) {
        GetThemePartSize(th, dc, BP_CHECKBOX, state, nullptr, TS_DRAW, &box);
        RECT br{r.left, r.top + (r.bottom - r.top - box.cy) / 2, r.left + box.cx, 0};
        br.bottom = br.top + box.cy;
        DrawThemeBackground(th, dc, BP_CHECKBOX, state, &br, nullptr);
        CloseThemeData(th);
    }
    const std::wstring text = textOf(h);
    HGDIOBJ oldFont = SelectObject(dc, fontOf(h));
    RECT tr{r.left + box.cx + box.cx / 2, r.top, r.right, r.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kDarkText : kDarkGrayText);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((cd->uItemState & CDIS_FOCUS) && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT fr{};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &fr, DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
        OffsetRect(&fr, tr.left, tr.top + ((tr.bottom - tr.top) - (fr.bottom - fr.top)) / 2);
        InflateRect(&fr, 1, 1);
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, oldFont);
    result = CDRF_SKIPDEFAULT;
    return true;
}

}  // namespace myalien::tray::theme
