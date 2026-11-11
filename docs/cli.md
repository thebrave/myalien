# `myalien` command reference

`myalien.exe` is the command-line front-end of MyAlien. It reads and changes the thermal and
battery-charging settings of a **Dell G15 5520** through firmware interfaces that Windows already
exposes (see [firmware.md](firmware.md)).

```
myalien <command> [arguments]
```

Running `myalien` with no command is the same as `myalien status`.

## Privileges and safety rules

* **Dell firmware access needs the MyAlien service or an elevated prompt.** From an elevated
  prompt, commands run in-process. From a normal prompt, reads and changes go through the service
  (see the end of this page); any logged-in user may change settings, with the same checks.
  Without either, the Dell sections show `unavailable (access denied …)` and changes are refused.
  Battery health, power source and Windows power mode always work.
* **Where `status` / `battery` data comes from** is shown on the `Source` line:
  `local (elevated)`, `MyAlien service` (not elevated, service running), or
  `local (not elevated; <reason>)`.
* **Writes are only enabled on a Dell G15 5520** (`SystemManufacturer = Dell Inc.`,
  `SystemProductName = Dell G15 5520`). Reads work anywhere the interfaces exist.
* **Every write is read back from the firmware** before the command reports `OK`.
* After a successful write, `Note : MyDell or the FusionService is already running.` is printed
  when Dell's `FusionService` (MyDell's backend) is running. It is informational only.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (or nothing to do) |
| 1 | Error: not elevated, wrong machine, firmware/WMI failure, feature unavailable |
| 2 | Usage error: unknown command, argument or value out of range |
| 3 | The write was accepted but the firmware does not report the requested state |

---

## `status`

Read-only overview: machine, elevation, MyDell service state, power source, Windows power mode,
thermal profile, G-Mode, temperatures and fans, battery health, charge mode.

```
myalien status
```

```
MyAlien 0.1.0 - Dell Inc. Dell G15 5520 (SKU 0B56, BIOS 1.44.0)
  Source        : MyAlien service
  MyDell        : FusionService running
  Power source  : AC
  Windows mode  : Balanced

Thermal (Dell AWCC WMI)
  Profile       : Balanced (0xA0)   [BIOS AWThermalManagement: Balanced]
  Available     : Balanced (0xA0), Balanced-Performance (0xA1), Low-Power (0xA5), Quiet (0xA3)
  G-Mode        : off   (Game Shift: off)
  CPU           :  51 °C   fan 0 rpm (max 4800), boost 0
  GPU           :  47 °C   fan 0 rpm (max 4800), boost 0

Battery
  Model         : DELL 70N2F32 (SMP, LiP)
  Health        : 79.2 %  (66.8 Wh of 84.3 Wh design)
  Charge        : 100 %  (idle, on AC)
  Charge mode   : Adaptive   (custom range 50-80 %, inactive)
  Modes         : Adaptive, Standard, Express, PrimAcUse, Custom
  Advanced mode : Disabled
  BIOS password : none
```

The `[BIOS AWThermalManagement: …]` value is a BIOS setting that mirrors the active profile.
A `note:` line appears if it ever disagrees with the profile.

---

## `profile`

```
myalien profile                  Show current and offered thermal profiles
myalien profile set <profile>    Activate a profile
```

`<profile>` is one of the names below, or a hex ID such as `0xA0`. Only profiles the firmware
offers are accepted, plus `custom`.

| Name | ID | Notes |
|---|---|---|
| `balanced` | `0xA0` | |
| `balanced-performance` | `0xA1` | |
| `quiet` | `0xA3` | |
| `low-power` | `0xA5` | BIOS shows it as "Battery" |
| `custom` | `0x00` | Not listed by the firmware but always available; enables `fan boost` |

Refused while G-Mode is on (use `myalien gmode off <profile>`). G-Mode itself is not set here.

```
> myalien profile set quiet
Before  : Low-Power (0xA5)  -> low-power
After   : Quiet (0xA3)  -> quiet
BIOS    : AWThermalManagement = Quiet
OK
```

---

## `gmode`

```
myalien gmode                    Show G-Mode state
myalien gmode on                 Turn G-Mode on
myalien gmode off [<profile>]    Turn G-Mode off and activate <profile> (default: balanced)
```

G-Mode is thermal profile `0xAB`: maximum cooling, both fans boosted to 100. "On" means the
current profile is `0xAB`. Game Shift is displayed for information; the firmware switches it by
itself together with G-Mode.

```
> myalien gmode on
Before  : G-Mode off, profile Quiet (0xA3)  -> quiet, Game Shift off
After   : G-Mode on, profile G-Mode (0xAB), Game Shift on
BIOS    : AWThermalManagement = Performance
OK
```

Reported by other G15 users (not seen here at idle): a ~1 s system freeze when switching, and
the firmware may keep G-Mode for about a minute while the machine is hot. If `gmode off` exits
with code 3, wait and run `myalien gmode` again.

---

## `fan`

```
myalien fan                          Show fan speed, boost and linked temperature
myalien fan boost <fan> <0-100>      Set a fan's boost (Custom profile only)
```

`<fan>` is `cpu`, `gpu` or `all`. Boost is a percentage of the fan's maximum speed (4800 rpm on
this machine); `0` means no extra boost, so the firmware manages the fan and may stop it.
Values above 100 are refused.

Switch to the Custom profile first:

```
> myalien profile set custom
> myalien fan boost all 30
Before  :
  CPU :    0 rpm (max 4800), boost   0, 57 °C
  GPU :    0 rpm (max 4800), boost   0, 46 °C
Set     : CPU boost = 30  -> reads 30
Set     : GPU boost = 30  -> reads 30
+1s     :  CPU 0 rpm  GPU 0 rpm
…
+4s     :  CPU 1320 rpm  GPU 1388 rpm
…
OK
```

The fans take 3–4 s to react, so the command samples their speed for 8 s after the write.

---

## `battery`

```
myalien battery                              Show battery health and charge mode
myalien battery mode <mode>                  Set the charge mode
myalien battery mode custom <start> <stop>   Charge only between start % and stop %
```

| `<mode>` | BIOS value | Meaning |
|---|---|---|
| `adaptive` | Adaptive | BIOS adapts charging to usage (Dell default) |
| `standard` | Standard | Normal full charge |
| `express` | Express | Fast charge (harder on the battery) |
| `primacuse`, `ac` | PrimAcUse | Mostly on AC: keeps the battery at a lower level |
| `custom` | Custom | Charge between `CustomChargeStart` and `CustomChargeStop` |

Custom range: start 50–95 %, stop 55–100 %, stop at least 5 above start (bounds are read from the
BIOS). `battery mode custom` without numbers keeps the stored range.

The settings live in the BIOS and **apply immediately** (no reboot). MyDell edits the same
settings. The command switches the mode to Custom before changing the thresholds (they are
read-only otherwise) and orders the two threshold writes so the 5 % window holds after each one.

```
> myalien battery mode custom 55 85
Before  : Adaptive (custom range 50-80 %, inactive)
Set     : PrimaryBattChargeCfg = Custom  -> reads Custom
Set     : CustomChargeStop = 85  -> reads 85
Set     : CustomChargeStart = 55  -> reads 55
After   : Custom (custom range 55-85 %)
OK
```

If a BIOS admin password is set, writes are refused (password support is not implemented).

---

## `powermode`

The Windows power mode slider (Settings > System > Power > Power mode). It is a per-user Windows
setting, not a firmware one: the command runs as you, never through the service, and needs no
elevation. The value applies to the current power source (AC or battery), like the Settings slider.

```
myalien powermode
myalien powermode set <efficiency | balanced | performance>
```

```
Before     : Balanced (on AC)
After      : Best performance
OK
```

---

## `games`

Game catalog for game mode ([game-mode.md](game-mode.md)). Runs as you, never through the
service. It reads launcher catalogs (Steam, Epic Games, Ubisoft Connect, Battle.net, GOG Galaxy,
EA app, Riot Client, Rockstar, Xbox app, Amazon Games) and Game Bar's list of games
(`GameConfigStore`). Nothing walks the disk: only known registry keys and launcher files are
read (~20 ms warm).

```
myalien games          List the games and whether the foreground window is one
myalien games watch    Print each foreground change and the verdict (Ctrl+C to stop)
```

```
Scanned in 18 ms: 13 games

  Steam            8
  Epic Games       0
  GOG Galaxy       not installed
  ...
  Game Bar         3

Caves of Qud                         Steam            C:\Program Files (x86)\Steam\steamapps\common\Caves of Qud
Red Dead Redemption                  Game Bar         C:\Games\Red Dead Redemption\RDR.exe
...

Foreground: C:\Users\…\Microsoft VS Code\Code.exe
    -> not a game
```

A launcher game matches any process under its install folder (prelaunchers, the real exe); a
Game Bar entry matches only its own executable. Launcher clients and crash handlers never match.
`watch` uses the same event as the tray will (`EVENT_SYSTEM_FOREGROUND`), so it costs nothing
while idle.

---

## `help`

```
myalien help        (also --help, -h, /?)
```

---

# `myalien-svc` — the MyAlien service

```
myalien-svc --install      Copy to %ProgramFiles%\MyAlien, register and start the service (elevated)
myalien-svc --uninstall    Stop and remove the service and the installed copy (elevated)
myalien-svc --console      Run the pipe server in the foreground (development, Ctrl+C to stop)
```

The service (`MyAlien`, LocalSystem, automatic start, restarts on failure) lets non-elevated
clients read and change the Dell settings over `\\.\pipe\MyAlien`, so every `myalien` command works
from a normal prompt. Reads show `Source : MyAlien service`; changes start with
`Via : MyAlien service`. Any logged-in user may change settings: the service applies the same
validation as the elevated CLI and logs each change with the caller's account and process ID.

The service also **remembers** the thermal profile and fan boosts last set through MyAlien
(`C:\Program Files\MyAlien\state.ini`) and checks the firmware every 15 s. When MyDell's
`FusionService` is not running, it re-applies that setting after boot or resume if the firmware lost
it. If anything else changes the setting (MyDell, the firmware, another tool), the service logs it
and stops re-applying until your next change through `myalien`. With MyDell running it only
observes. Writes are sent to the service even from an elevated prompt, so it knows about them.

Reading the BIOS charge settings takes ~5 s, so the service keeps them in memory (refreshed at
start-up, after resume, every 10 minutes). `status` shows how old they are
(`Settings read : … ago by the service`) and answers in ~0.3 s; `battery` asks the service to
re-read them first and takes ~5 s.

* `--install` copies the executable to `C:\Program Files\MyAlien\myalien-svc.exe` (a SYSTEM service
  must not run from a user-writable folder such as the build tree). Running it again updates an
  installed service: it stops it, replaces the executable and starts it.
* Log: `C:\Program Files\MyAlien\logs\service.log` (rotated at 1 MB).
* `--console` runs as the current user. Clients refuse it by default because it is not the
  registered service; set `MYALIEN_TRUST_ANY_SERVER=1` in the client's environment to talk to it.

---

# `myalien-tray` — tray icon

Start `myalien-tray.exe` as yourself (not elevated); it needs the MyAlien service.

* **Icon**: a coloured letter for the current profile — **Q** Quiet, **B** Balanced,
  **P** Balanced-Performance, **L** Low-Power, **C** Custom, **G** G-Mode; a grey **?** when the
  service cannot be reached; **G** also while Game Shift is stuck. The tooltip shows the profile and
  CPU / GPU temperatures and fan speeds (refreshed every 6 s, every 2 s while the window is open).
* **Left click** opens the **status window**:
  * *Thermal*: profile list, G-Mode checkbox (also shows and fixes a stuck Game Shift), CPU / GPU
    temperature, fan speed and boost, and **boost sliders** with *Apply boost* (Custom profile only).
  * *Battery*: model, charge, health, **Charging** mode list, **custom range** (start – stop %) with
    *Use range* (switches to Custom with that range), and how old the charge settings are.
  * *Games*: the **game mode** checkbox, the **thermal profile** and **power mode** to use while a
    game is in front (each can be *Leave unchanged*), **Only on AC**, a line saying what game mode
    is doing now, and **Games (N)…**, which opens the Games window. This group works without the
    service; only the thermal profile needs it.
  * The bottom line shows the result of the last change. Closing the window only hides it.
* **Games window**: every game found (name, launcher, folder or program). Untick a row to never
  treat it as a game (e.g. a browser in Game Bar's list). **Add folder…** adds a game no launcher
  lists (any program in that folder counts); **Remove** takes it off again. **Rescan** reads the
  list now; it is also re-read by itself when a launcher installs or removes a game. The footer
  counts games per launcher.
* **Right click** opens the **menu**: profile (radio items), G-Mode on/off, battery level and health,
  **Charging** submenu, **Game mode: switch automatically while a game is in front** (and, while a game has engaged it, a `Game: <name> → …` line),
  *Open MyAlien…*, **Start with Windows**, Refresh, Exit.
* **Game mode** (off by default, [game-mode.md](game-mode.md)): while a game from the catalog
  (`myalien games`) is in the foreground, the tray sets the thermal profile (through the service)
  and the Windows power mode (itself); default G-Mode + Best performance, on AC only. About 8 s
  after the game leaves the foreground, each value is restored unless something else changed it
  meanwhile. *Exit* restores at once; after a crash or logoff, the next start restores. Only the
  first switch after turning it on shows a notification; failures always do. Settings live in
  `HKCU\Software\MyAlien\GameMode` (per user).
* Changes go through the service like `myalien` commands. Notifications: a change failed, a
  charge-mode change completed (when the window is closed), **the profile was changed by something
  else** (e.g. MyDell), or **Game Shift got stuck** (fans at full boost).
* Only one tray icon runs per session.
