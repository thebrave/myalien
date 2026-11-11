// `myalien battery [mode <mode> [<start> <stop>]]` - battery health and BIOS charge mode.

#include "cli/commands.h"
#include "cli/common.h"
#include "core/actions.h"
#include "core/wmi.h"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien battery                              Show battery health and charge mode\n"
        "  myalien battery mode <mode>                  Set the charge mode\n"
        "  myalien battery mode custom <start> <stop>   Charge only between start % and stop %\n"
        "\n"
        "<mode>: adaptive | standard | express | primacuse (mostly on AC) | custom\n"
        "Custom range: start 50-95, stop 55-100, stop at least 5 above start.\n"
        "The setting is stored in the BIOS; MyDell shows the same setting.\n"
        "Changes run through the MyAlien service, or in-process from an elevated prompt.\n",
        stderr);
    return exitcode::Usage;
}

}  // namespace

int cmdBattery(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;

    if (args.empty()) {
        const auto [snapshot, source] = obtainSnapshot({.thermal = false}, /*freshCharge=*/true);
        std::printf("Source        : %s\n", source.c_str());
        printBatterySection(snapshot);
        return exitcode::Ok;
    }

    if (args[0] == L"mode" && (args.size() == 2 || args.size() == 4)) {
        const std::string mode = toAscii(args[1]);
        std::optional<std::pair<std::string, std::string>> range;
        std::string request = "CHARGE " + mode;
        if (args.size() == 4) {
            range = std::make_pair(toAscii(args[2]), toAscii(args[3]));
            request += " " + range->first + " " + range->second;
        }
        // Each BIOS write is followed by a full attribute read (~1.5 s): allow time for up to 3.
        return runWrite("Changing the charge mode", request,
                        [&] { return actSetChargeMode(mode, range).output; }, 40000);
    }
    return usage();
}

}  // namespace myalien::cli
