// myalien-tray.exe - MyAlien tray icon (runs as the logged-in user, talks to the MyAlien service).

#include "tray/theme.h"
#include "tray/tray_app.h"

#include <windows.h>
#include <commctrl.h>

// Common Controls v6 (visual styles for the status window), via a linker-generated manifest.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);
    myalien::tray::theme::initApp();  // dark menus and windows when Windows uses the dark app theme

    // One tray icon per user session.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MyAlienTray");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    myalien::tray::TrayApp app;
    const int r = app.run(instance);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return r;
}
