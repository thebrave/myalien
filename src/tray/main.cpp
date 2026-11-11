// myalien-tray.exe - MyAlien tray icon (runs as the logged-in user, talks to the MyAlien service).

#include "tray/tray_app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
