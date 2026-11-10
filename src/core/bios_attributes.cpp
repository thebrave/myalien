#include "core/bios_attributes.h"

#include "core/error.h"

#include <cwctype>
#include <format>
#include <stdexcept>

namespace myalien {

namespace {

// Attribute names are compile-time constants, but keep WQL injection impossible anyway.
std::wstring whereName(const wchar_t* field, const wchar_t* name) {
    for (const wchar_t* p = name; *p; ++p)
        if (!std::iswalnum(*p) && *p != L'_') throw std::invalid_argument("invalid BIOS attribute name");
    return std::wstring(L" WHERE ") + field + L"='" + name + L"'";
}

bool isAllowedWrite(const wchar_t* name) {
    const std::wstring_view n(name);
    return n == attr::PrimaryBattChargeCfg || n == attr::CustomChargeStart || n == attr::CustomChargeStop;
}

// SetAttribute Status codes (as mapped by the Linux dell-wmi-sysman driver).
const char* setStatusText(int64_t s) {
    switch (s) {
        case 1: return "failed";
        case 2: return "invalid parameter";
        case 3: return "access denied (BIOS password?)";
        case 4: return "not supported";
        case 5: return "memory error";
        case 6: return "protocol error";
        default: return "unknown status";
    }
}

void putBstr(IWbemClassObject* in, const wchar_t* prop, const std::wstring& value) {
    wmi::ScopedVariant v;
    v.get()->vt = VT_BSTR;
    v.get()->bstrVal = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    if (!v.get()->bstrVal) throw std::bad_alloc();
    check(in->Put(prop, 0, v.get(), 0), "SetAttribute: Put string");
}

void putUInt32(IWbemClassObject* in, const wchar_t* prop, uint32_t value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;  // CIM uint32 is marshalled as VT_I4
    v.lVal = static_cast<LONG>(value);
    check(in->Put(prop, 0, &v, 0), "SetAttribute: Put uint32");
}

void putEmptyBytes(IWbemClassObject* in, const wchar_t* prop) {
    wmi::ScopedVariant v;
    v.get()->vt = VT_ARRAY | VT_UI1;
    v.get()->parray = SafeArrayCreateVector(VT_UI1, 0, 0);
    if (!v.get()->parray) throw std::bad_alloc();
    check(in->Put(prop, 0, v.get(), 0), "SetAttribute: Put uint8[]");
}

}  // namespace

const wchar_t* expectedAwThermalManagement(uint8_t wmaxProfile) {
    switch (wmaxProfile) {
        case 0xA0: return L"Balanced";
        case 0xA5: return L"Battery";
        case 0xA3: return L"Quiet";
        case 0xA1: return L"Performance";
        case 0xAB: return L"Performance";  // G-Mode shares the value with Balanced-Performance
        case 0x00: return L"Balanced";     // Custom shares the value with Balanced
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

void BiosAttributes::set(const wchar_t* name, const std::wstring& value) const {
    if (!isAllowedWrite(name))
        throw std::logic_error(std::format("BIOS: refusing to write non allow-listed attribute {}", toUtf8(name)));
    if (adminPasswordSet().value_or(false))
        throw std::runtime_error("BIOS: an admin password is set; password-protected writes are not supported yet");

    std::wstring path;
    for (const auto& o : ns_.query(L"SELECT * FROM BIOSAttributeInterface")) {
        if (auto p = o.getString(L"__RELPATH")) {
            path = *p;
            break;
        }
    }
    if (path.empty()) throw std::runtime_error("BIOS: no BIOSAttributeInterface instance");

    const auto cls = ns_.getClass(L"BIOSAttributeInterface");
    wmi::ComPtr<IWbemClassObject> inDef;
    check(cls.get()->GetMethod(L"SetAttribute", 0, &inDef, nullptr), "BIOS GetMethod(SetAttribute)");
    wmi::ComPtr<IWbemClassObject> in;
    check(inDef->SpawnInstance(0, &in), "BIOS SpawnInstance");

    putBstr(in.Get(), L"AttributeName", name);
    putBstr(in.Get(), L"AttributeValue", value);
    putUInt32(in.Get(), L"SecType", 0);      // no password
    putUInt32(in.Get(), L"SecHndCount", 0);  // no security handle
    putEmptyBytes(in.Get(), L"SecHandle");

    const auto out = ns_.execMethod(path, L"SetAttribute", in.Get());
    const auto status = out.getInt(L"Status");
    if (!status) throw std::runtime_error("BIOS: SetAttribute returned no Status");
    if (*status != 0)
        throw std::runtime_error(std::format("BIOS: SetAttribute {}={} failed: {} (Status {})", toUtf8(name),
                                             toUtf8(value), setStatusText(*status), *status));
}

}  // namespace myalien
