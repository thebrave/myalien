#pragma once

// Thermal state as the service tracks it, and the setting the user last chose through MyAlien
// ("desired" state), persisted in <install dir>\state.ini (administrators-only location).

#include "core/awcc.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace myalien::svc {

struct ThermalState {
    uint8_t profile = 0;
    std::map<uint8_t, uint32_t> boosts;  // fan id -> boost
    // As read from the firmware; not persisted (a remembered setting implies Game Shift on for
    // G-Mode, off otherwise).
    std::optional<bool> gameShift;

    bool gameShiftOn() const { return gameShift.value_or(profile == profile::GMode); }
};

// Current profile, fan boosts and Game Shift as the firmware reports them.
ThermalState readThermalState(Awcc& awcc);

// "Quiet (0xA3)", "Custom (0x00), boost CPU 30 GPU 30", "Quiet (0xA3), Game Shift stuck on".
std::string describe(const ThermalState& s);

// Same user-visible setting: same profile, same Game Shift state (a stuck Game Shift keeps the
// fans at full boost), and in Custom the same boosts (in other profiles the firmware manages the
// fans, e.g. G-Mode sets boost 100 by itself).
bool sameSetting(const ThermalState& a, const ThermalState& b);

struct DesiredState {
    std::optional<ThermalState> thermal;  // last setting made through MyAlien
    // Something else changed the setting afterwards (MyDell, firmware, another tool): MyAlien
    // stops re-applying until the next change made through it.
    bool overridden = false;
};

DesiredState loadDesired(const std::wstring& iniPath);
void saveDesired(const std::wstring& iniPath, const DesiredState& d);

// <directory of this executable>\state.ini
std::wstring defaultStatePath();

}  // namespace myalien::svc
