#include "tray/about.h"

#include "tray/icon.h"

#include <commctrl.h>
#include <shellapi.h>

namespace myalien::tray {

namespace {

HRESULT CALLBACK onEvent(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<const wchar_t*>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

}  // namespace

void showAbout(HWND owner, HINSTANCE instance) {
    TASKDIALOGCONFIG c{sizeof(c)};
    c.hwndParent = owner;
    c.hInstance = instance;
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    c.pszWindowTitle = L"About MyAlien";
    c.hMainIcon = appIcon(instance, false);
    c.pszMainInstruction = L"MyAlien " MYALIEN_VERSION;
    c.pszContent =
        L"Thermal and battery care for the Dell G15 5520, without Dell's software.\n\n"
        L"Not made or endorsed by Dell. MIT License.\n"
        L"<a href=\"" MYALIEN_PROJECT_URL L"\">" MYALIEN_PROJECT_URL L"</a>";
    c.pfCallback = onEvent;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}

}  // namespace myalien::tray
