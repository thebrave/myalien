// `myalien profile [set <name>]` - read or change the Dell thermal profile.

#include "cli/commands.h"
#include "cli/common.h"
#include "core/awcc.h"
#include "core/error.h"
#include "core/wmi.h"

#include <cstdio>

namespace myalien::cli {

namespace {

void printProfiles(Awcc& awcc) {
    const uint8_t cur = awcc.currentProfile();
    std::printf("Current : %s\n", describeProfile(cur).c_str());
    std::printf("Offered :\n");
    for (uint8_t p : awcc.caps().profiles)
        std::printf("  %c %s\n", p == cur ? '*' : ' ', describeProfile(p).c_str());
    if (awcc.caps().gameShift) std::printf("G-Mode  : %s\n", cur == profile::GMode ? "on" : "off");
}

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien profile                 Show current and offered thermal profiles\n"
        "  myalien profile set <profile>   Activate a profile (elevated prompt required)\n"
        "\n"
        "<profile>: balanced | balanced-performance | quiet | low-power | custom, or a hex ID such as 0xA0.\n"
        "Only profiles offered by the firmware (plus custom) are accepted. For G-Mode use `myalien gmode`.\n"
        "In custom, set fan speeds with `myalien fan boost`.\n",
        stderr);
    return 2;
}

int setProfile(std::wstring_view arg) {
    const auto target = parseProfile(arg);
    if (!target) {
        std::fprintf(stderr, "Unknown profile: %s\n\n", toUtf8(arg).c_str());
        return usage();
    }
    if (const auto refused = refuseWrite("Changing the thermal profile")) return *refused;

    Awcc awcc;
    const uint8_t before = awcc.currentProfile();
    if (before == profile::GMode) {
        std::fputs("G-Mode is on. Use `myalien gmode off <profile>` to leave it.\n", stderr);
        return 1;
    }

    std::printf("Before  : %s\n", describeProfile(before).c_str());
    if (before == *target) {
        std::puts("Already active, nothing to do.");
        return 0;
    }

    awcc.setProfile(*target);

    // Read back from the hardware: the firmware, not our write, is the source of truth.
    const uint8_t after = awcc.currentProfile();
    std::printf("After   : %s\n", describeProfile(after).c_str());
    printAwThermalAfterWrite(after);
    noteMyDell();

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
