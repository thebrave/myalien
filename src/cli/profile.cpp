// `myalien profile [set <name>]` - read or change the Dell thermal profile.

#include "cli/commands.h"
#include "cli/common.h"
#include "core/actions.h"
#include "core/awcc.h"
#include "core/wmi.h"

#include <cstdio>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien profile                 Show current and offered thermal profiles\n"
        "  myalien profile set <profile>   Activate a profile\n"
        "\n"
        "<profile>: balanced | balanced-performance | quiet | low-power | custom, or a hex ID such as 0xA0.\n"
        "Only profiles offered by the firmware (plus custom) are accepted. For G-Mode use `myalien gmode`.\n"
        "In custom, set fan speeds with `myalien fan boost`.\n"
        "Changes run through the MyAlien service, or in-process from an elevated prompt.\n",
        stderr);
    return exitcode::Usage;
}

int show() {
    const auto [s, source] = obtainSnapshot({.battery = false, .charge = false});
    if (s.thermalError) {
        std::fprintf(stderr, "Thermal data unavailable: %s\n", s.thermalError.message.c_str());
        return exitcode::Error;
    }
    std::printf("Source  : %s\n", source.c_str());
    std::printf("Current : %s\n", describeProfile(s.profile).c_str());
    std::printf("Offered :\n");
    for (uint8_t p : s.profiles) std::printf("  %c %s\n", p == s.profile ? '*' : ' ', describeProfile(p).c_str());
    std::printf("  %c %s  (always available)\n", s.profile == profile::Custom ? '*' : ' ',
                describeProfile(profile::Custom).c_str());
    if (s.gameShiftSupported) std::printf("G-Mode  : %s\n", s.profile == profile::GMode ? "on" : "off");
    return exitcode::Ok;
}

}  // namespace

int cmdProfile(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;
    if (args.empty()) return show();
    if (args[0] == L"set" && args.size() == 2) {
        const std::string name = toAscii(args[1]);
        return runWrite("Changing the thermal profile", "PROFILE " + name, [&] { return actSetProfile(name); });
    }
    return usage();
}

}  // namespace myalien::cli
