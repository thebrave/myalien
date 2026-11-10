// `myalien gmode [on | off [<profile>]]` - G-Mode (Game Shift + profile 0xAB).

#include "cli/commands.h"
#include "cli/common.h"
#include "core/awcc.h"
#include "core/error.h"
#include "core/wmi.h"

#include <cstdio>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien gmode                   Show G-Mode state\n"
        "  myalien gmode on                Turn G-Mode on (elevated prompt required)\n"
        "  myalien gmode off [<profile>]   Turn G-Mode off and activate <profile> (default: balanced)\n"
        "\n"
        "<profile>: balanced | balanced-performance | quiet | low-power, or a hex ID such as 0xA0.\n",
        stderr);
    return 2;
}

void printState(Awcc& awcc, const char* label) {
    const auto gs = awcc.gameShift();
    const uint8_t p = awcc.currentProfile();
    std::printf("%-8s: G-Mode %s, profile %s\n", label, gs ? (*gs ? "on" : "off") : "unknown",
                describeProfile(p).c_str());
}

int setGMode(bool on, uint8_t profileWhenOff) {
    if (const auto refused = refuseWrite("Changing G-Mode")) return *refused;

    Awcc awcc;
    if (!awcc.caps().gameShift) {
        std::fputs("G-Mode is not supported on this machine.\n", stderr);
        return 1;
    }

    printState(awcc, "Before");
    const auto gsBefore = awcc.gameShift();
    const uint8_t pBefore = awcc.currentProfile();
    const uint8_t wanted = on ? profile::GMode : profileWhenOff;
    if (gsBefore && *gsBefore == on && pBefore == wanted) {
        std::puts("Already in that state, nothing to do.");
        return 0;
    }

    awcc.setGMode(on, profileWhenOff);

    // Read back both halves of the state from the firmware.
    const auto gsAfter = awcc.gameShift();
    const uint8_t pAfter = awcc.currentProfile();
    printState(awcc, "After");
    printAwThermalAfterWrite(pAfter);
    noteMyDell();

    if (!gsAfter || *gsAfter != on || pAfter != wanted) {
        std::fputs("G-Mode did not end up in the requested state (see After).\n", stderr);
        return 3;
    }
    std::puts("OK");
    return 0;
}

}  // namespace

int cmdGMode(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;

    if (args.empty()) {
        Awcc awcc;
        if (!awcc.caps().gameShift) {
            std::puts("G-Mode  : not supported on this machine");
            return 0;
        }
        printState(awcc, "Current");
        return 0;
    }

    if (args[0] == L"on" && args.size() == 1) return setGMode(true, profile::Balanced);

    if (args[0] == L"off" && args.size() <= 2) {
        uint8_t target = profile::Balanced;
        if (args.size() == 2) {
            const auto p = parseProfile(args[1]);
            if (!p) {
                std::fprintf(stderr, "Unknown profile: %s\n\n", toUtf8(args[1]).c_str());
                return usage();
            }
            target = *p;
        }
        return setGMode(false, target);
    }
    return usage();
}

}  // namespace myalien::cli
