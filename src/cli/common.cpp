#include "cli/common.h"

#include "core/bios_attributes.h"
#include "core/error.h"
#include "core/system.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <format>

namespace myalien::cli {

namespace {

struct ProfileAlias {
    const wchar_t* name;
    uint8_t id;
};

// CLI names for regular profiles. Only those the firmware offers are accepted by the core.
constexpr ProfileAlias kAliases[] = {
    {L"balanced", profile::Balanced},
    {L"balanced-performance", profile::BalancedPerformance},
    {L"quiet", profile::Quiet},
    {L"low-power", profile::LowPower},
    {L"cool", profile::Cool},
    {L"performance", profile::Performance},
};

std::wstring lower(std::wstring_view s) {
    std::wstring r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
}

const wchar_t* aliasOf(uint8_t id) {
    for (const auto& a : kAliases)
        if (a.id == id) return a.name;
    return nullptr;
}

}  // namespace

std::optional<uint8_t> parseProfile(std::wstring_view arg) {
    const std::wstring s = lower(arg);
    for (const auto& a : kAliases)
        if (s == a.name) return a.id;
    if (s.size() == 4 && s.starts_with(L"0x")) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(s.c_str(), &end, 16);
        if (end && *end == L'\0' && v <= 0xFF) return static_cast<uint8_t>(v);
    }
    return std::nullopt;
}

std::string describeProfile(uint8_t id) {
    const wchar_t* alias = aliasOf(id);
    return std::format("{} (0x{:02X}){}", profileName(id), id, alias ? "  -> " + toUtf8(alias) : std::string());
}

std::optional<std::wstring> readAwThermal() {
    try {
        BiosAttributes bios;
        if (const auto a = bios.enumeration(attr::AWThermalManagement)) return a->current;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

void printAwThermalAfterWrite(uint8_t profile) {
    const wchar_t* expected = expectedAwThermalManagement(profile);
    std::optional<std::wstring> aw;
    for (int i = 0; i < 10; ++i) {
        aw = readAwThermal();
        if (!aw || !expected || *aw == expected) break;
        Sleep(200);
    }
    if (!aw) return;
    std::string suffix;
    if (expected && *aw != expected) suffix = std::format("  (expected {})", toUtf8(expected));
    std::printf("BIOS    : AWThermalManagement = %s%s\n", toUtf8(*aw).c_str(), suffix.c_str());
}

std::optional<int> refuseWrite(const char* what) {
    const auto machine = machineInfo();
    if (!machine.isG15_5520()) {
        std::fprintf(stderr, "Refusing to write: this machine is %s %s, writes are only enabled on a Dell G15 5520.\n",
                     toUtf8(machine.manufacturer).c_str(), toUtf8(machine.product).c_str());
        return 1;
    }
    if (!isElevated()) {
        std::fprintf(stderr, "%s requires an elevated prompt (Run as administrator).\n", what);
        return 1;
    }
    return std::nullopt;
}

void noteMyDell() {
    if (fusionServiceState() == ServiceState::Running)
        std::puts("Note    : MyDell or the FusionService is already running.");
}

}  // namespace myalien::cli
