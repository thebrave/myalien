// `myalien powermode [set <mode>]` - Windows power mode slider. Runs as the user; never uses the
// service (it is a per-user Windows setting, not a firmware one).

#include "cli/commands.h"
#include "core/actions.h"
#include "core/error.h"
#include "core/system.h"

#include <windows.h>

#include <cstdio>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien powermode              Show the Windows power mode\n"
        "  myalien powermode set <mode>   Set it: efficiency | balanced | performance\n",
        stderr);
    return exitcode::Usage;
}

}  // namespace

int cmdPowerMode(std::span<const std::wstring_view> args) {
    const auto source = [] {
        const auto ac = onAcPower();
        return !ac ? "unknown power source" : *ac ? "on AC" : "on battery";
    };
    if (args.empty()) {
        std::printf("Power mode : %s (%s)\n", toString(windowsPowerMode()), source());
        return exitcode::Ok;
    }
    if (args.size() != 2 || args[0] != L"set") return usage();
    PowerMode target;
    if (args[1] == L"efficiency") target = PowerMode::BestEfficiency;
    else if (args[1] == L"balanced") target = PowerMode::Balanced;
    else if (args[1] == L"performance") target = PowerMode::BestPerformance;
    else return usage();

    const PowerMode before = windowsPowerMode();
    std::printf("Before     : %s (%s)\n", toString(before), source());
    if (const unsigned long rc = setWindowsPowerMode(target); rc != 0) {
        std::fprintf(stderr, "error: PowerSetActiveOverlayScheme failed: %s (%lu)\n",
                     hrMessage(HRESULT_FROM_WIN32(rc)).c_str(), rc);
        return exitcode::Error;
    }
    const PowerMode after = windowsPowerMode();
    std::printf("After      : %s\n", toString(after));
    if (after != target) {
        std::fputs("error: Windows does not report the requested mode.\n", stderr);
        return exitcode::NotApplied;
    }
    std::puts("OK");
    return exitcode::Ok;
}

}  // namespace myalien::cli
