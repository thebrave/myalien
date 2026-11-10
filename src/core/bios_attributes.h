#pragma once

// Dell BIOS attributes over WMI: root\dcim\sysman\biosattributes (EnumerationAttribute,
// IntegerAttribute, BIOSAttributeInterface) and root\dcim\sysman\wmisecurity (PasswordObject).
// Spec reference: Linux drivers/platform/x86/dell/dell-wmi-sysman (interface facts only).
//
// This module is read-only for now; writes (SetAttribute) come with Phase 1 write commands.

#include "core/wmi.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myalien {

struct EnumAttribute {
    std::wstring name;
    std::wstring current;
    std::vector<std::wstring> possible;
    std::wstring modifiers;  // e.g. "[ReadOnlyIfNot:PrimaryBattChargeCfg=Custom]"
    bool readOnly = false;
};

struct IntAttribute {
    std::wstring name;
    int64_t current = 0;
    int64_t lower = 0;
    int64_t upper = 0;
    int64_t step = 1;
    std::wstring modifiers;
    bool readOnly = false;
};

namespace attr {
inline constexpr wchar_t PrimaryBattChargeCfg[] = L"PrimaryBattChargeCfg";
inline constexpr wchar_t CustomChargeStart[] = L"CustomChargeStart";
inline constexpr wchar_t CustomChargeStop[] = L"CustomChargeStop";
inline constexpr wchar_t AdvBatteryChargeCfg[] = L"AdvBatteryChargeCfg";
inline constexpr wchar_t AWThermalManagement[] = L"AWThermalManagement";
}  // namespace attr

// AWThermalManagement value observed to mirror each active WMAX profile on the G15 5520
// (PLAN.md §1b, all confirmed by writes). nullptr when no mirror is known.
// Not a reverse map: G-Mode (0xAB) and Balanced-Performance (0xA1) both show "Performance".
const wchar_t* expectedAwThermalManagement(uint8_t wmaxProfile);

class BiosAttributes {
public:
    BiosAttributes();

    std::optional<EnumAttribute> enumeration(const wchar_t* name) const;
    std::optional<IntAttribute> integer(const wchar_t* name) const;

    // nullopt when the security namespace is unavailable.
    std::optional<bool> adminPasswordSet() const;

private:
    wmi::Namespace ns_;
};

}  // namespace myalien
