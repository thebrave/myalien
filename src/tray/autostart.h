#pragma once

// "Start with Windows": HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MyAlien = "<this exe>".

namespace myalien::tray {

bool autostartEnabled();
bool setAutostart(bool enable);

}  // namespace myalien::tray
