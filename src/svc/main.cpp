// myalien-svc.exe - MyAlien Windows service.

#include "svc/service.h"

#include <windows.h>

#include <cstdio>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring_view arg = argc > 1 ? argv[1] : L"";

    if (arg.empty()) return myalien::svc::runAsService();
    if (arg == L"--install") return myalien::svc::install();
    if (arg == L"--uninstall") return myalien::svc::uninstall();
    if (arg == L"--console") return myalien::svc::runInConsole();

    std::fputs(
        "MyAlien service " MYALIEN_VERSION "\n"
        "\n"
        "Usage: myalien-svc [--install | --uninstall | --console]\n"
        "\n"
        "  --install     Copy to %ProgramFiles%\\MyAlien, register and start the service (elevated)\n"
        "  --uninstall   Stop and remove the service and the installed copy (elevated)\n"
        "  --console     Run the pipe server in this console (development; Ctrl+C to stop)\n"
        "\n"
        "Without arguments it expects to be started by the Service Control Manager.\n",
        arg == L"--help" || arg == L"-h" || arg == L"/?" ? stdout : stderr);
    return arg == L"--help" || arg == L"-h" || arg == L"/?" ? 0 : 2;
}
