# MyAlien

Lightweight thermal and battery care for the **Dell G15 5520**: a small alternative to MyDell /
Alienware Command Center, limited to thermal profiles, fans and battery charging.

It talks to the firmware only through interfaces Windows already exposes (the inbox ACPI-WMI driver,
the battery class driver, power APIs). No Dell DLLs, no direct hardware access. It works with or
without MyDell installed.

Status: **Phase 1 done** — a command-line tool, every command verified on a G15 5520.
Phase 2 in progress — with the Windows service installed, every command works without elevation.
Next: MyDell coexistence and re-apply after resume, then a tray app (see [PLAN.md](PLAN.md)).

## What it can do

| Command | |
|---|---|
| `myalien status` | Profile, G-Mode, temperatures, fans, battery health, charge mode |
| `myalien profile set <balanced\|balanced-performance\|quiet\|low-power\|custom>` | Thermal profile |
| `myalien gmode on` / `gmode off [<profile>]` | G-Mode (maximum cooling) |
| `myalien fan boost <cpu\|gpu\|all> <0-100>` | Manual fan boost, in the Custom profile |
| `myalien battery mode <adaptive\|standard\|express\|primacuse>` | BIOS charge mode |
| `myalien battery mode custom <start> <stop>` | Charge only between two levels, e.g. `50 80` |

Changes go through the MyAlien service (or need an **elevated prompt** without it) and are only
enabled on a Dell G15 5520. Each one is read back from the firmware before the command reports `OK`. Full reference: [docs/cli.md](docs/cli.md).

## Build

Requires Visual Studio 2022 (Desktop development with C++); CMake ships with it.

```
cmake --preset vs2022
cmake --build --preset release
ctest --test-dir build/vs2022 -C Release
```

Output: `build/vs2022/src/cli/Release/myalien.exe` and `build/vs2022/src/svc/Release/myalien-svc.exe`
(single self-contained files, static CRT). Install the service with `myalien-svc --install` from an
elevated prompt (see [docs/cli.md](docs/cli.md)).
If `cmake` is not on `PATH`, use a "Developer PowerShell for VS 2022" or VS's bundled
`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

## Documentation

| Document | Content |
|---|---|
| [docs/cli.md](docs/cli.md) | Command reference, examples, exit codes |
| [docs/architecture.md](docs/architecture.md) | Code layout, core library, safety model |
| [docs/firmware.md](docs/firmware.md) | Dell WMI interfaces used and what was verified on the G15 5520 |
| [PLAN.md](PLAN.md) | Project plan, phases, discovery notes, open questions |

## Layout

| Path | Content |
|---|---|
| `src/core` | `myalien-core` static library: WMI wrapper, Dell AWCC thermal interface, Dell BIOS attributes, charge planner, battery, system queries |
| `src/cli` | `myalien.exe` command line |
| `src/svc` | `myalien-svc.exe` Windows service (pipe server) |
| `tests` | Unit tests (charge planner, snapshot wire format) |
| `tools/discovery` | Phase 0 read-only discovery script |
| `docs` | Documentation; `docs/discovery` holds local discovery dumps (git-ignored) |

## License

MIT — see [LICENSE](LICENSE). Linux drivers and other GPL projects were used as interface
documentation only; no code was copied from them.
