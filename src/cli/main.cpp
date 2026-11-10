// myalien.exe - command line front-end for MyAlien (Phase 1 test bench).

#include "cli/commands.h"
#include "core/error.h"

#include <windows.h>

#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

namespace {

void printUsage() {
    std::fputs(
        "MyAlien " MYALIEN_VERSION " - thermal & battery care for the Dell G15 5520\n"
        "\n"
        "Usage: myalien <command>\n"
        "\n"
        "Commands:\n"
        "  status                Show thermal profile, temperatures, fans, battery and charge settings (read-only)\n"
        "  profile               Show current and offered thermal profiles\n"
        "  profile set <name>    Activate a thermal profile: balanced | balanced-performance | quiet | low-power\n"
        "  gmode                 Show G-Mode state\n"
        "  gmode on              Turn G-Mode on\n"
        "  gmode off [<name>]    Turn G-Mode off and activate a profile (default: balanced)\n"
        "  help                  Show this help\n"
        "\n"
        "Dell firmware interfaces (thermal, BIOS settings) require an elevated prompt.\n",
        stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::wstring_view> args(argv + 1, argv + argc);
    const std::wstring_view cmd = args.empty() ? L"status" : args.front();
    const std::span<const std::wstring_view> rest =
        args.empty() ? std::span<const std::wstring_view>{} : std::span<const std::wstring_view>(args).subspan(1);

    try {
        if (cmd == L"status") return myalien::cli::cmdStatus(rest);
        if (cmd == L"profile") return myalien::cli::cmdProfile(rest);
        if (cmd == L"gmode") return myalien::cli::cmdGMode(rest);
        if (cmd == L"help" || cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
            printUsage();
            return 0;
        }
        std::fprintf(stderr, "Unknown command: %s\n\n", myalien::toUtf8(cmd).c_str());
        printUsage();
        return 2;
    } catch (const myalien::HrError& e) {
        if (e.accessDenied())
            std::fputs("error: access denied - the Dell firmware interfaces need an elevated prompt (Run as administrator).\n",
                       stderr);
        else
            std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
