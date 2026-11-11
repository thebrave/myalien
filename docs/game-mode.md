# Detecting a running game

Research notes and design for game mode (Phase 5 in [PLAN.md](../PLAN.md)): notice that a game
is in the foreground, then switch the Windows power mode **and** the thermal profile. Nothing here
is implemented. The constraints are the ones in [architecture.md](architecture.md): C++20,
Windows SDK only, no third-party code.

The power-mode change goes through `PowerGetEffectiveOverlayScheme` /
`PowerSetActiveOverlayScheme` in `src/core/system.cpp`. The thermal profile change uses the
existing `PROFILE` / `GMODE` pipe requests. The feature is opt-in, with or without
`FusionService`. The [Design](#design) section at the end covers behaviour and UI.

## Windows has no “is a game” API

Android (`isGame`) and iOS expose a system classification an outside app can trust. Windows does
not. What exists is either in-process, restricted, or incomplete:

* **`HasExpandedResources`**
  ([`expandedresources.h`](https://learn.microsoft.com/en-us/windows/win32/api/expandedresources/nf-expandedresources-hasexpandedresources))
  reports whether *this process* currently has Game Mode resources. It is only meaningful inside
  the game. The Game Mode APIs are deprecated since Windows 10 1809. An observer cannot call it
  on another process.
* **`Windows.Gaming.Preview.GamesEnumeration.GameList`** needs the restricted `gameList`
  capability. Normal desktop apps fail at runtime.
* **Game Bar Presence Writer**
  ([docs](https://learn.microsoft.com/en-us/windows/win32/devnotes/gamebar-presencewriter))
  is the component Windows starts when a *known* game is in the foreground
  (`GameBarPresenceWriter.exe`, registered under
  `HKLM\SOFTWARE\Microsoft\WindowsRuntime\Server\Windows.Gaming.GameBar.Internal.PresenceWriterServer\ExePath`).
  Watching that process is Windows’ own verdict, and it needs no game list. Measured limits
  (Windows 11 25H2,
  [GameModeExecutor](https://github.com/geeooff/GameModeExecutor/blob/main/docs/design/00-detection.md)):
  it only starts for titles on Microsoft’s Known Game List (an Xbox TitleId). A game the user
  only marked with Game Bar’s “Remember this is a game” updates `GameConfigStore` and does not
  start the writer. The writer also does not say which game.
* **`SHQueryUserNotificationState` → `QUNS_RUNNING_D3D_FULL_SCREEN`** is exclusive-fullscreen
  Direct3D only. Most current games are borderless windowed, so this misses them.
* **`HKCU\System\GameConfigStore\Children\{guid}` → `MatchedExeFullPath`** is the list of
  executables Windows has classified (Known Game List plus anything the user told Game Bar to
  remember). It is not a live “running now” flag. Entries appear after a game has been seen, and
  browsers sometimes land in the list. `UtmItemId` can hold a stale path; trust
  `MatchedExeFullPath`.
* **Steam only:** `HKCU\Software\Valve\Steam` → `RunningAppID` is non-zero while a Steam app is
  running. It does not cover other launchers.

## Practical signal

Collect per-game install directories (and the exe, when the launcher records one), then watch the
foreground process from the tray:

1. `GetForegroundWindow` → `GetWindowThreadProcessId` → `QueryFullProcessImageName`.
2. Treat it as a game when that image path is inside a collected install directory and is not the
   launcher itself (`steam.exe`, `EpicGamesLauncher.exe`, `upc.exe`, `Battle.net.exe`,
   `GalaxyClient.exe`, `EADesktop.exe`, `RiotClientServices.exe`, Rockstar’s `Launcher.exe`,
   Amazon’s UI, the Xbox app).
3. Optionally union `MatchedExeFullPath` for games outside those launchers (loose installs,
   HoYoPlay, itch). That list is per-user and only fills in after Windows has seen the exe.

The tray is the right process. `myalien-svc.exe` runs as LocalSystem, and `GameConfigStore`,
Steam’s `RunningAppID`, and Amazon’s database live in the interactive user’s hive or profile.
The tray also sets the power mode itself (a per-user Windows setting, no elevation expected; to
confirm). Only the thermal profile needs the service, through requests it already accepts.

Match the **directory**, not a single exe. Several titles start a prelauncher
(`REDprelauncher.exe`) and then the real game. Anti-cheat and crash handlers also live in that
folder, so the check is “foreground process under this directory”, not “any process under this
directory”.

```
launcher catalogs ──► install directories ──┐
GameConfigStore MatchedExeFullPath ─────────┼──► foreground image path?
manual folders − exclusions ────────────────┘         │
                                                      ├── yes → tray sets power mode
                                                      │         tray sends PROFILE / GMODE → service
                                                      └── no (after debounce) → restore both
```

## Launcher install paths

“Parent path” is not one folder per store. Steam is the only launcher with an explicit list of
library roots. The others record each game’s own install directory. Use the per-game directory
for detection; a library root is only a way to find those directories.

Paths below were checked against current library scanners (Playnite, GameFinder, and similar).
Launchers move these files. A missing key or file means that launcher is not installed.

### Steam

* Client: `HKLM\SOFTWARE\WOW6432Node\Valve\Steam` → `InstallPath`. Fallback:
  `HKCU\Software\Valve\Steam` → `SteamPath` (forward slashes).
* Library roots: `<InstallPath>\steamapps\libraryfolders.vdf`. Current files have one block per
  library with a `"path"` string (escaped backslashes). Always include the Steam install
  directory itself; it is a library even when the VDF is old or missing.
* Each game: `<library>\steamapps\appmanifest_<appid>.acf` (plain text, `"key" "value"`).
  `installdir` is a folder name, not a full path. The game directory is
  `<library>\steamapps\common\<installdir>`. `name` is the title.
* Skip non-games by appid (Steamworks redistributables such as `228980`) and by name
  (`Steamworks Common Redistributables`).
* Live signal, Steam only: `HKCU\Software\Valve\Steam` → `RunningAppID`.

### Epic Games

There is no library-root index. Each install is a JSON manifest:

* `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item`
* `InstallLocation` is the game directory. `LaunchExecutable` is relative to it
  (`InstallLocation` + that path is the exe). `DisplayName`, `AppName`.
* Skip when `bIsIncompleteInstall` is true, and skip DLC: `MainGameAppName` set and different
  from `AppName`.
* Launcher location, not games: `HKLM\SOFTWARE\WOW6432Node\Epic Games\EpicGamesLauncher` →
  `AppDataPath`.

These files are readable without elevation.

### Ubisoft Connect

* Per game: `HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\<numericId>` → `InstallDir`
  (slash-separated; normalize).
* Display name is not on that key. Read `DisplayName` from
  `HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Uplay Install <id>`.
  Fall back to the folder name.
* The launcher does not record the exe. Match any foreground image under `InstallDir`, excluding
  `upc.exe`.
* `%LOCALAPPDATA%\Ubisoft Game Launcher\settings.yaml` has `game_installation_path`, the default
  games folder. Games can be installed elsewhere, so `InstallDir` is the list to trust.

### Battle.net

The canonical catalog is `%ProgramData%\Battle.net\Agent\product.db`, a protobuf
(`ProductInstall`: product code, `settings.install_path`, installed flag). Parsing it means
vendoring a schema. The Win32-only equivalent is the uninstall registry:

* `HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*` and the 64-bit
  `Uninstall` key.
* Keep entries whose `UninstallString` matches `Battle.net` and `--uid=<id>`.
* `InstallLocation` is the game directory. `DisplayName` is the title.
* Drop the Battle.net client itself, and names ending in `Test`, `Beta`, or containing `PTR`.

### GOG Galaxy

Galaxy 2 still writes one registry key per installed GOG game. That avoids SQLite:

* `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<productId>`
* `path` — install directory. `exe` — full executable (sometimes a prelauncher, so still match
  the directory). `gameName`. `dependsOn` non-empty means DLC; skip it.
* Launcher binary: `HKLM\SOFTWARE\WOW6432Node\GOG.com\GalaxyClient\paths` → `client`, plus
  `clientExecutable` on `GOG.com\GalaxyClient`.
* `%ProgramData%\GOG.com\Galaxy\storage\galaxy-2.0.db` is the full Galaxy library, including
  games only linked from other stores. Install paths are spread across `PlayTasks` and related
  tables, and the schema moves. The registry already covers games Galaxy itself installed.

Each GOG folder also contains `goggame-<id>.info` when the registry key is missing.

### Amazon Games

Amazon stopped writing a registry catalog around 2022.

* `%LOCALAPPDATA%\Amazon Games\Data\Games\Sql\GameInstallInfo.sqlite`
* Table `DbSet`: `Id`, `ProductTitle`, `InstallDirectory`, filter `Installed = 1`.
* Exe: `<InstallDirectory>\fuel.json` → `Main.Command` (relative exe name).
* The database lives in the interactive user’s profile, so the tray would read it. It is SQLite:
  read it with the inbox `winsqlite3.dll` (`winsqlite/winsqlite3.h` in the Windows SDK), opened
  read-only with `immutable=1` in case the client holds it. Fallback: a user-chosen library
  folder whose immediate subfolders are treated as games (misses a custom layout). `installQueue.json` beside `Games\` is an older catalog and is not the current one.

### EA App

EA’s own library file (`%LOCALAPPDATA%\Electronic Arts\EA Desktop\…`) is encrypted. The install
directory is in the registry, including games the EA App installed:

* `HKLM\SOFTWARE\EA Games\<GameName>` and `HKLM\SOFTWARE\WOW6432Node\EA Games\<GameName>` →
  `Install Dir` (note the space; the value usually ends with a backslash).
* Fallback: uninstall entries whose `UninstallString` contains `EAInstaller`. `InstallLocation`
  is the game directory.
* Skip the EA App itself. Old Origin installs used `C:\Program Files (x86)\Origin Games`; those
  still show up under the same `EA Games` keys when the game is registered.

### Riot Client

Valorant, League of Legends, Teamfight Tactics, Legends of Runeterra. Either source is enough;
the YAML is the one that exists even when the user declined shortcuts:

* `%ProgramData%\Riot Games\Metadata\<product>.live\<product>.live.product_settings.yaml` →
  `product_install_full_path` (present only when that patchline is installed).
  `product_install_root` is the shared Riot folder, not the game. Examples: `valorant.live`,
  `league_of_legends.live`. Ignore patchlines other than `live` (PBE).
* Uninstall subkeys named `Riot Game <product>.live` (per-machine or per-user).
  `InstallLocation` is the game directory. Riot omits this key when the user declined shortcuts.
* Client binary, not a game: `%ProgramData%\Riot Games\RiotClientInstalls.json`.

### Rockstar Games Launcher

Few titles, but GTA V and Red Dead Redemption 2 are large. One key per game:

* `HKLM\SOFTWARE\WOW6432Node\Rockstar Games\<title>` → `InstallFolder`.
* Skip subkeys whose name contains `Launcher` or `Social Club`.

### Xbox app / PC Game Pass

The Windows-native store. `%ProgramFiles%\WindowsApps` is ACL-locked and often only a junction.
The Xbox app writes a library root on each drive it uses:

* Hidden file `<drive>\.GamingRoot`. Header bytes `RGBX`, then a count, then UTF-16 folder names
  relative to that drive (commonly `XboxGames`), each NUL-terminated. Ignore the file if it is
  large or does not start with `RGBX`.
* Also check `<drive>\XboxGames` when the file is missing.
* Each child directory that contains `Content\MicrosoftGame.config` is one game. The config is
  XML; the display name is in it. Match the foreground process against that game directory.

## Launchers left out

* **HoYoPlay** (Genshin, Honkai, Zenless) is common, but the catalog is a binary `gamedata.dat`
  under `%APPDATA%\Cognosphere\HYP\<version>\data\`. The old standalone miHoYo launcher used a
  plain `config.ini` key `game_install_path`; HoYoPlay does not. `MatchedExeFullPath` covers
  these after one launch.
* **itch.io**: `%APPDATA%\itch\db\butler.db` (SQLite `caves.verdict` JSON with `basePath`).
  Indie-only, and the same SQLite problem as Amazon.
* **Heroic**: a third-party client for Epic, GOG, and Amazon
  (`%APPDATA%\heroic\…\installed.json`). It only matters when the official launcher is not
  installed.
* **Minecraft launcher**: Java is one directory (`%APPDATA%\.minecraft`). Bedrock is an Xbox
  install and is covered by `.GamingRoot`.
* Single-franchise or regional clients (Jagex, Wargaming Game Center, Nexon, NCSOFT, Krafton)
  and retired ones (Bethesda.net, Twitch, Glyph). If a title is missed later, the same pattern
  applies: a registry `InstallLocation`, or Game Bar’s remembered exe.

## Design

Decided 2026-11-11: game mode changes **both** the Windows power mode and the thermal profile,
and everything that can run as the user runs in the tray. The service stays as it is.

### Who does what

| Part | Process | Why |
|---|---|---|
| Launcher catalogs, `GameConfigStore` | tray (`core/games`, also used by `myalien games`) | per-user hive and profile; no elevation |
| Foreground watch | tray, `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` | event-driven, no polling; needs the user's desktop |
| Windows power mode | tray, `PowerSetActiveOverlayScheme` | per-user setting; to confirm non-elevated |
| Thermal profile / G-Mode | service, via the existing `PROFILE` / `GMODE` requests | WMAX needs admin; no new pipe message |
| Settings (on/off, targets, exclusions, manual folders, pending restore) | tray, `HKCU\Software\MyAlien\GameMode` | per user |

Because the profile change is an ordinary MyAlien write, the service records it in `state.ini`,
logs it with the caller, and the conflict guard sees it as MyAlien's own change.

### Behaviour

* **Enter**: when the foreground process is a game, remember the current power mode and profile,
  then set the targets. No delay, so the game starts in the right mode.
* **Leave**: wait about 8 s after the game loses the foreground (alt-tab to Discord, a launcher
  overlay, a lock screen). Coming back to the same or another game cancels the restore.
* **Restore only what MyAlien set**: before restoring, read the current values. If the user (tray,
  `Fn+F9`, MyDell, Settings) changed the power mode or the profile during the game, leave that
  one alone. Each part is decided on its own.
* **Crash-safe**: the values to restore are written to `HKCU` before switching and cleared after
  restoring. At tray start, a pending restore with no game in front is applied. Exiting the tray
  during a game restores first.
* **Targets**: power mode Best efficiency / Balanced / Best performance / unchanged; thermal
  profile any enumerated profile, G-Mode, or unchanged. Defaults: Best performance + G-Mode.
* **FusionService running**: the feature still works when the user opted in, but the status
  window says that Fusion may switch the profile back. The conflict guard reports it if it does.
* **Open**: only on AC power (see PLAN §8).

### UI

**Tray menu**, between the battery lines and “Open MyAlien…”:

```
Game: Elden Ring → G-Mode, Best performance     (grayed, only while a game is focused)
✓ Game mode
```

The tooltip adds `· Elden Ring` while a game is focused. Notifications only when a switch fails
and the first time game mode acts after it was turned on, not on every alt-tab.

**Status window**, a third group under Battery (about 130 px taller):

```
┌ Games ─────────────────────────────────────────────────┐
│ [✓] While a game is in the foreground:                  │
│     Power mode        [Best performance ▾]              │
│     Thermal profile   [G-Mode           ▾]              │
│ Now: Elden Ring (Steam), was Balanced / Quiet           │
│                                           [Games… (142)]│
└─────────────────────────────────────────────────────────┘
```

The “Now:” line shows `No game in focus`, the focused game with the values it will restore, or a
warning (FusionService running, a failed switch).

**“Games…” dialog**: one ListView (report view, checkboxes) with Name, Launcher, Folder.
Unticking a row excludes that folder (browsers that end up in `GameConfigStore`); exclusions are
stored by path and survive a rescan. Buttons: Add folder… (games no catalog finds: HoYoPlay, itch,
Amazon, loose installs), Remove (manual folders only), Rescan, and a filter box. A footer counts
games per source. The catalog is scanned at tray start and on Rescan.

**CLI**: `myalien games` prints the catalog and whether the current foreground process matches;
`myalien powermode [set <mode>]` reads or sets the Windows power mode directly. Both run as the
user and never use the service.

### Implementation notes (increment 2)

* The foreground hook skips the tray's own windows (`WINEVENT_SKIPOWNPROCESS`): opening the MyAlien
  menu or status window during a game does not count as leaving it.
* Every 2 s the tray re-checks only while a game is focused or game mode is engaged (leave delay,
  AC unplugged with “only on AC”). Otherwise nothing runs between foreground changes.
* A restore that needs the thermal profile waits for the service to answer; the pending record
  stays in `HKCU\Software\MyAlien\GameMode\Pending` until then.
