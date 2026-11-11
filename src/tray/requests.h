#pragma once

// Pipe requests built from the UI state, shared by the tray menu and the status window.

#include "core/snapshot.h"

#include <cstdint>
#include <string>

namespace myalien::tray {

struct Request {
    std::string text;  // pipe request, e.g. "PROFILE 0xA3"
    std::string what;  // user-facing description
};

// Switch to `target` (a firmware profile or Custom). Leaving G-Mode, or a stuck Game Shift,
// goes through GMODE OFF so the service records the right setting and clears Game Shift.
Request profileRequest(const Snapshot& s, uint8_t target);

// Toggle G-Mode. Off returns to `lastRegular`, or to the profile the firmware shows when Game
// Shift is stuck.
Request gmodeToggleRequest(const Snapshot& s, uint8_t lastRegular);

// Switch to any profile, G-Mode included (game mode's enter and restore).
Request switchRequest(const Snapshot& s, uint8_t target);

// "Primarily AC" for PrimAcUse, the BIOS name otherwise.
std::wstring chargeModeLabel(const std::string& mode);

}  // namespace myalien::tray
