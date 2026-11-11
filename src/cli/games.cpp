// `myalien games [watch]` - game catalog for game mode, and live foreground detection.
// Runs as the user; never uses the service.

#include "cli/commands.h"
#include "core/actions.h"
#include "core/error.h"
#include "core/games.h"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <memory>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien games          List installed games found in launcher catalogs and Game Bar\n"
        "  myalien games watch    Report each foreground change and whether it is a game (Ctrl+C to stop)\n",
        stderr);
    return exitcode::Usage;
}

std::string verdict(const games::Matcher& m, const std::wstring& image) {
    if (image.empty()) return "(no foreground process, or access denied)";
    const games::Game* g = m.match(image);
    return toUtf8(image) + (g ? "\n    -> GAME: " + toUtf8(g->name) + " (" + games::toString(g->source) + ")"
                              : std::string("\n    -> not a game"));
}

int list() {
    const auto cat = games::scan();
    std::printf("Scanned in %.0f ms: %zu games\n\n", cat.milliseconds, cat.games.size());
    for (const auto& r : cat.sources) {
        std::printf("  %-16s %s", games::toString(r.source), r.present ? "" : "not installed");
        if (r.present) std::printf("%d", r.games);
        if (!r.error.empty()) std::printf("   (%s)", r.error.c_str());
        std::putchar('\n');
    }
    std::putchar('\n');
    for (const auto& g : cat.games) {
        std::printf("%-36s %-16s %s\n", toUtf8(g.name).c_str(), games::toString(g.source),
                    toUtf8(g.source == games::Source::GameBar ? g.exe : g.dir).c_str());
    }
    const games::Matcher m(cat.games);
    std::printf("\nForeground: %s\n", verdict(m, games::foregroundImage()).c_str());
    return exitcode::Ok;
}

const games::Matcher* gWatchMatcher = nullptr;
std::wstring gLastImage;

void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    const std::wstring image = games::foregroundImage();
    if (image == gLastImage) return;
    gLastImage = image;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    std::printf("%s  %s\n", ts, verdict(*gWatchMatcher, image).c_str());
    std::fflush(stdout);
}

int watch() {
    const auto cat = games::scan();
    const games::Matcher m(cat.games);
    gWatchMatcher = &m;
    std::printf("%zu games known (scan %.0f ms). Switch windows to test; Ctrl+C to stop.\n\n", cat.games.size(),
                cat.milliseconds);
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0,
                                         0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) throwLastError("SetWinEventHook");
    onForeground(nullptr, 0, nullptr, 0, 0, 0, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    UnhookWinEvent(hook);
    return exitcode::Ok;
}

}  // namespace

int cmdGames(std::span<const std::wstring_view> args) {
    if (args.empty()) return list();
    if (args.size() == 1 && args[0] == L"watch") return watch();
    return usage();
}

}  // namespace myalien::cli
