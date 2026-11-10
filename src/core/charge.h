#pragma once

// Battery charge configuration (Dell BIOS attributes PrimaryBattChargeCfg, CustomChargeStart,
// CustomChargeStop) and the planning of a change into a safe sequence of attribute writes.
//
// Rules (BIOS attribute metadata on the G15 5520, same limits as Linux dell-laptop):
//  * CustomChargeStart / CustomChargeStop are "[ReadOnlyIfNot:PrimaryBattChargeCfg=Custom]"
//    -> switch the mode to Custom before touching them;
//  * start in [50, 95], stop in [55, 100] (bounds read from the BIOS), stop - start >= 5,
//    and that difference must hold after every single write.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myalien {

class BiosAttributes;

inline constexpr int64_t kMinChargeWindow = 5;
inline constexpr wchar_t kCustomMode[] = L"Custom";

struct ChargeSettings {
    std::wstring mode;                // e.g. L"Adaptive"
    std::vector<std::wstring> modes;  // as offered by the BIOS
    int64_t start = 0, startMin = 0, startMax = 0;
    int64_t stop = 0, stopMin = 0, stopMax = 0;
};

struct ChargeRange {
    int64_t start = 0;
    int64_t stop = 0;
};

struct AttributeWrite {
    const wchar_t* name;
    std::wstring value;
};

// Reads the three attributes. Throws std::runtime_error if the BIOS does not expose them.
ChargeSettings readChargeSettings(const BiosAttributes& bios);

// Validates the request and returns the ordered writes to perform (empty when nothing changes).
// `mode` must be one of settings.modes (exact BIOS spelling). A range is only allowed with Custom.
// Throws std::invalid_argument with a user-facing message.
std::vector<AttributeWrite> planChargeChange(const ChargeSettings& current, const std::wstring& mode,
                                             const std::optional<ChargeRange>& range);

}  // namespace myalien
