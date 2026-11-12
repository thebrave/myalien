#include "tray/theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cwchar>
#include <initializer_list>
#include <iterator>

namespace myalien::tray::theme {

namespace {

// Windows 11 dark palette (as in Settings / Explorer).
constexpr COLORREF kDarkWindow = RGB(32, 32, 32);
constexpr COLORREF kDarkControl = RGB(45, 45, 45);
constexpr COLORREF kDarkText = RGB(240, 240, 240);

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

void themeControl(HWND h, bool isDark) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, static_cast<int>(std::size(cls)));
    if (ux().allowDarkModeForWindow) ux().allowDarkModeForWindow(h, isDark);

    if (_wcsicmp(cls, WC_BUTTONW) == 0) {
        // Check boxes and group boxes draw their text in black whatever the theme: in dark mode
        // they use classic drawing, which honours WM_CTLCOLORBTN / STATIC.
        if (hasStyle(h, BS_TYPEMASK, {BS_CHECKBOX, BS_AUTOCHECKBOX, BS_GROUPBOX}))
            SetWindowTheme(h, isDark ? L"" : nullptr, isDark ? L"" : nullptr);
        else
            SetWindowTheme(h, isDark ? L"DarkMode_Explorer" : nullptr, nullptr);
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
