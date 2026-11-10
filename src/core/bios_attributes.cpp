#include "core/bios_attributes.h"

#include "core/error.h"

#include <cwctype>
#include <stdexcept>

namespace myalien {

namespace {

// Attribute names are compile-time constants, but keep WQL injection impossible anyway.
std::wstring whereName(const wchar_t* field, const wchar_t* name) {
    for (const wchar_t* p = name; *p; ++p)
        if (!std::iswalnum(*p) && *p != L'_') throw std::invalid_argument("invalid BIOS attribute name");
    return std::wstring(L" WHERE ") + field + L"='" + name + L"'";
}

}  // namespace

const wchar_t* expectedAwThermalManagement(uint8_t wmaxProfile) {
    switch (wmaxProfile) {
        case 0xA0: return L"Balanced";
        case 0xA5: return L"Battery";
        case 0xA3: return L"Quiet";
        case 0xA1: return L"Performance";
        default: return nullptr;
    }
}

BiosAttributes::BiosAttributes() : ns_(wmi::Namespace::connect(L"ROOT\\DCIM\\SYSMAN\\BIOSATTRIBUTES")) {}

std::optional<EnumAttribute> BiosAttributes::enumeration(const wchar_t* name) const {
    const auto rows = ns_.query(L"SELECT * FROM EnumerationAttribute" + whereName(L"AttributeName", name));
    if (rows.empty()) return std::nullopt;
    const auto& o = rows.front();
    EnumAttribute a;
    a.name = name;
    a.current = o.getString(L"CurrentValue").value_or(L"");
    a.possible = o.getStringArray(L"PossibleValue");
    a.modifiers = o.getString(L"Modifiers").value_or(L"");
    a.readOnly = o.getInt(L"ReadOnly").value_or(0) != 0;
    return a;
}

std::optional<IntAttribute> BiosAttributes::integer(const wchar_t* name) const {
    const auto rows = ns_.query(L"SELECT * FROM IntegerAttribute" + whereName(L"AttributeName", name));
    if (rows.empty()) return std::nullopt;
    const auto& o = rows.front();
    IntAttribute a;
    a.name = name;
    a.current = o.getInt(L"CurrentValue").value_or(0);
    a.lower = o.getInt(L"LowerBound").value_or(0);
    a.upper = o.getInt(L"UpperBound").value_or(0);
    a.step = o.getInt(L"ScalarIncrement").value_or(1);
    a.modifiers = o.getString(L"Modifiers").value_or(L"");
    a.readOnly = o.getInt(L"ReadOnly").value_or(0) != 0;
    return a;
}

std::optional<bool> BiosAttributes::adminPasswordSet() const {
    try {
        const auto sec = wmi::Namespace::connect(L"ROOT\\DCIM\\SYSMAN\\WMISECURITY");
        const auto rows = sec.query(L"SELECT * FROM PasswordObject" + whereName(L"NameId", L"Admin"));
        if (rows.empty()) return std::nullopt;
        return rows.front().getInt(L"IsPasswordSet").value_or(0) != 0;
    } catch (const HrError&) {
        return std::nullopt;
    }
}

}  // namespace myalien
