#pragma once

// Dell BIOS attributes over WMI: root\dcim\sysman\biosattributes (EnumerationAttribute,
// IntegerAttribute, BIOSAttributeInterface) and root\dcim\sysman\wmisecurity (PasswordObject).
// Spec reference: Linux drivers/platform/x86/dell/dell-wmi-sysman (interface facts only).
//
// Writes go through BIOSAttributeInterface.SetAttribute and are limited to an allow-list
// (battery charge settings only).

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
// Not a reverse map: G-Mode (0xAB) and Balanced-Performance (0xA1) both show "Performance",
// Custom (0x00) and Balanced (0xA0) both show "Balanced".
const wchar_t* expectedAwThermalManagement(uint8_t wmaxProfile);

class BiosAttributes {
public:
    BiosAttributes();

    std::optional<EnumAttribute> enumeration(const wchar_t* name) const;
    std::optional<IntAttribute> integer(const wchar_t* name) const;

    // nullopt when the security namespace is unavailable.
    std::optional<bool> adminPasswordSet() const;

    // Writes one allow-listed attribute (PrimaryBattChargeCfg, CustomChargeStart, CustomChargeStop).
    // No BIOS password support: refuses when an admin password is set.
    // Throws std::logic_error (not allow-listed), std::runtime_error (non-zero Status), HrError (WMI).
    void set(const wchar_t* name, const std::wstring& value) const;

private:
    wmi::Namespace ns_;
};

}  // namespace myalien
