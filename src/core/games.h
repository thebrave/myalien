#pragma once

// Game catalog for game mode (see docs/game-mode.md): installed games read from launcher
// catalogs and from Game Bar's list, and a matcher that decides whether a process image is
// one of them. Runs as the interactive user; no elevation, no disk walk, no third-party code.

#include <string>
#include <vector>

namespace myalien::games {

enum class RegRoot { LocalMachine, CurrentUser };  // avoids <windows.h> in this header

enum class Source { Steam, Epic, Ubisoft, BattleNet, Gog, Ea, Riot, Rockstar, Xbox, Amazon, GameBar, Manual };
const char* toString(Source s);

struct Game {
    std::wstring name;
    Source source = Source::Manual;
    std::wstring dir;  // install directory, normalized (full path, backslashes, no trailing '\')
    std::wstring exe;  // full path when known; for Game Bar entries it is what gets matched
    std::wstring id;   // launcher id (Steam appid, Epic AppName, ...) when there is one
};

struct SourceReport {
    Source source;
    bool present = false;  // the launcher (or its catalog) was found
    int games = 0;
    std::string error;     // non-fatal read problem, for diagnostics
};

// A place whose change may mean a game was installed or removed.
struct WatchTarget {
    enum class Kind { Directory, RegistryKey } kind = Kind::Directory;
    RegRoot root = RegRoot::LocalMachine;  // RegistryKey only
    std::wstring path;
    bool contentWrites = false;  // Directory: also report file writes (a database), not only names
};

struct Catalog {
    std::vector<Game> games;  // deduplicated, sorted by name
    std::vector<SourceReport> sources;
    std::vector<WatchTarget> watch;  // only places that exist
    double milliseconds = 0;  // scan duration
};

// Reads every launcher catalog. Directories that no longer exist are dropped.
Catalog scan();

// Full path, backslashes, no trailing separator. Empty for an empty input.
std::wstring normalizePath(std::wstring path);

// Decides whether a process image belongs to a game. Pure: no I/O.
//  * Launcher entries match any image under their directory.
//  * Game Bar entries match their exact executable.
//  * Launcher clients (steam.exe, upc.exe, ...) never match.
//  * Drive roots are never used as a game directory.
class Matcher {
public:
    explicit Matcher(std::vector<Game> games);
    const Game* match(const std::wstring& imagePath) const;
    const std::vector<Game>& games() const { return games_; }

private:
    std::vector<Game> games_;
};

// Image path of the process that owns the foreground window, or "" (no window, access denied).
std::wstring foregroundImage();

}  // namespace myalien::games
