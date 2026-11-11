#include "tray/requests.h"

#include "core/awcc.h"
#include "core/error.h"

#include <format>

namespace myalien::tray {

Request profileRequest(const Snapshot& s, uint8_t target) {
    const std::string what = "Thermal profile: " + std::string(profileName(target));
    const bool inGMode = s.profile == profile::GMode || gameShiftStuck(s.profile, s.gameShift);
    // GMODE OFF only accepts firmware profiles; Custom goes through PROFILE (which also repairs a
    // stuck Game Shift).
    if (inGMode && target != profile::Custom) return {std::format("GMODE OFF 0x{:02X}", target), what};
    return {std::format("PROFILE 0x{:02X}", target), what};
}

Request gmodeToggleRequest(const Snapshot& s, uint8_t lastRegular) {
    const bool stuck = gameShiftStuck(s.profile, s.gameShift);
    if (s.profile != profile::GMode && !stuck) return {"GMODE ON", "G-Mode on"};
    const uint8_t back = stuck ? s.profile : lastRegular;
    if (back == profile::Custom) return {"PROFILE 0x00", "G-Mode off"};
    return {std::format("GMODE OFF 0x{:02X}", back), "G-Mode off"};
}

std::wstring chargeModeLabel(const std::string& mode) {
    if (mode == "PrimAcUse") return L"Primarily AC";
    return toWide(mode);
}

}  // namespace myalien::tray
