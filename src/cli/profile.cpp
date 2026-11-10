// `myalien profile [set <name>]` - read or change the Dell thermal profile.

#include "cli/commands.h"
#include "core/awcc.h"
#include "core/bios_attributes.h"
#include "core/error.h"
#include "core/system.h"
#include "core/wmi.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <format>
#include <optional>
#include <string>

namespace myalien::cli {

namespace {

struct ProfileAlias {
    const wchar_t* name;
    uint8_t id;
};

// CLI names for the profiles a G15 5520 can offer (USTT family). Hex IDs (0xA0) are accepted too.
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

const wchar_t* aliasOf(uint8_t id) {
    for (const auto& a : kAliases)
        if (a.id == id) return a.name;
    return nullptr;
}

std::string describe(uint8_t id) {
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

void printProfiles(Awcc& awcc) {
    const uint8_t cur = awcc.currentProfile();
    std::printf("Current : %s\n", describe(cur).c_str());
    std::printf("Offered :\n");
    for (uint8_t p : awcc.caps().profiles)
        std::printf("  %c %s\n", p == cur ? '*' : ' ', describe(p).c_str());
    if (const auto gs = awcc.gameShift()) std::printf("G-Mode  : %s\n", *gs ? "on" : "off");
}

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien profile                 Show current and offered thermal profiles\n"
        "  myalien profile set <profile>   Activate a profile (elevated prompt required)\n"
        "\n"
        "<profile>: balanced | balanced-performance | quiet | low-power, or a hex ID such as 0xA0.\n"
        "Only profiles offered by the firmware are accepted.\n",
        stderr);
    return 2;
}

int setProfile(std::wstring_view arg) {
    const auto target = parseProfile(arg);
    if (!target) {
        std::fprintf(stderr, "Unknown profile: %s\n\n", toUtf8(arg).c_str());
        return usage();
    }

    const auto machine = machineInfo();
    if (!machine.isG15_5520()) {
        std::fprintf(stderr, "Refusing to write: this machine is %s %s, writes are only enabled on a Dell G15 5520.\n",
                     toUtf8(machine.manufacturer).c_str(), toUtf8(machine.product).c_str());
        return 1;
    }
    if (!isElevated()) {
        std::fputs("Changing the thermal profile requires an elevated prompt (Run as administrator).\n", stderr);
        return 1;
    }

    Awcc awcc;
    if (const auto gs = awcc.gameShift(); gs && *gs) {
        std::fputs("G-Mode is on. Turn it off first (in MyDell for now; `myalien gmode off` comes next).\n", stderr);
        return 1;
    }

    const uint8_t before = awcc.currentProfile();
    std::printf("Before  : %s\n", describe(before).c_str());
    if (before == *target) {
        std::puts("Already active, nothing to do.");
        return 0;
    }

    awcc.setProfile(*target);

    // Read back from the hardware: the firmware, not our write, is the source of truth.
    const uint8_t after = awcc.currentProfile();
    std::printf("After   : %s\n", describe(after).c_str());

    // AWThermalManagement has been seen to mirror the profile; give it a moment to follow.
    if (const wchar_t* expected = expectedAwThermalManagement(after)) {
        std::optional<std::wstring> aw;
        for (int i = 0; i < 10; ++i) {
            aw = readAwThermal();
            if (!aw || *aw == expected) break;
            Sleep(200);
        }
        if (aw)
            std::printf("BIOS    : AWThermalManagement = %s%s\n", toUtf8(*aw).c_str(),
                        *aw == expected ? "" : std::format("  (expected {})", toUtf8(expected)).c_str());
    }

    if (fusionServiceState() == ServiceState::Running)
        std::puts("Note    : MyDell is running; its window may show the old profile until refreshed.");

    if (after != *target) {
        std::fprintf(stderr, "Profile did not change as requested (firmware reports 0x%02X).\n", after);
        return 3;
    }
    std::puts("OK");
    return 0;
}

}  // namespace

int cmdProfile(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;

    if (args.empty()) {
        Awcc awcc;
        printProfiles(awcc);
        return 0;
    }
    if (args[0] == L"set" && args.size() == 2) return setProfile(args[1]);
    return usage();
}

}  // namespace myalien::cli
