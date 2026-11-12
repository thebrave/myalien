#pragma once

// "About MyAlien": version, license and the project page.

#include <windows.h>

#define MYALIEN_PROJECT_URL L"https://github.com/thebrave/myalien"

namespace myalien::tray {

// Small modal dialog (TaskDialog) with the version, the license and a link to the project page.
void showAbout(HWND owner, HINSTANCE instance);

}  // namespace myalien::tray
