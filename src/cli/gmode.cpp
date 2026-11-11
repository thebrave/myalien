// `myalien gmode [on | off [<profile>]]` - G-Mode (thermal profile 0xAB).

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
        "  myalien gmode                   Show G-Mode state\n"
        "  myalien gmode on                Turn G-Mode on\n"
        "  myalien gmode off [<profile>]   Turn G-Mode off and activate <profile> (default: balanced)\n"
        "\n"
        "<profile>: balanced | balanced-performance | quiet | low-power, or a hex ID such as 0xA0.\n"
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
    if (!s.gameShiftSupported) {
        std::puts("G-Mode  : not supported on this machine");
        return exitcode::Ok;
    }
    const bool stuck = gameShiftStuck(s.profile, s.gameShift);
    std::printf("Current : G-Mode %s, profile %s, Game Shift %s\n",
                s.profile == profile::GMode ? "on" : stuck ? "STUCK (fans at full boost)" : "off",
                describeProfile(s.profile).c_str(), s.gameShift ? (*s.gameShift ? "on" : "off") : "n/a");
    if (stuck) std::puts("Fix     : myalien gmode off <profile>");
    return exitcode::Ok;
}

}  // namespace

int cmdGMode(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;
    if (args.empty()) return show();
    if (args[0] == L"on" && args.size() == 1)
        return runWrite("Changing G-Mode", "GMODE ON", [] { return actSetGMode(true); });
    if (args[0] == L"off" && args.size() <= 2) {
        const std::string target = args.size() == 2 ? toAscii(args[1]) : "balanced";
        return runWrite("Changing G-Mode", "GMODE OFF " + target, [&] { return actSetGMode(false, target); });
    }
    return usage();
}

}  // namespace myalien::cli
