#pragma once

// Light / dark appearance for the tray's windows and menus, following Windows' app theme
// (Settings > Personalization > Colors > "Choose your app mode").
//
// Plain Win32 controls have no dark theme of their own. This uses what Explorer and Notepad use:
// the dark title bar (DWM), uxtheme's "DarkMode_*" visual styles for buttons, lists and combo
// boxes, WM_CTLCOLOR* colours for backgrounds and text, and uxtheme's (unnamed) preferred-app-
// mode export so popup menus turn dark. Anything missing on an older Windows simply stays light.

#include <windows.h>

namespace myalien::tray::theme {

// Once at start-up, before any window exists.
void initApp();

// Windows' current app theme (re-read on each call; cheap).
bool dark();

// After WM_SETTINGCHANGE "ImmersiveColorSet": re-read the theme for menus.
void refresh();

// Title bar and child controls of a top-level window, for the current theme. Call after the
// controls exist, and again on a theme change.
void apply(HWND window);

// WM_ERASEBKGND: paints the window background; true when handled.
bool eraseBackground(HWND window, HDC dc);

// WM_CTLCOLORSTATIC / BTN / EDIT / LISTBOX: brush to return, or nullptr for the default.
HBRUSH controlColor(UINT msg, HDC dc);

// WM_NOTIFY from a child: draws check boxes in dark mode. True (with `result` set) when handled.
bool customDraw(LPARAM lp, LRESULT& result);

// True for a WM_SETTINGCHANGE that means the light / dark theme may have changed.
bool isThemeChange(UINT msg, LPARAM lp);

}  // namespace myalien::tray::theme
