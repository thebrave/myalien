# MyAlien

Lightweight thermal and battery care for the **Dell G15 5520**: a small alternative to MyDell /
Alienware Command Center, limited to thermal profiles, fans and battery charging.

It talks to the firmware only through interfaces Windows already exposes (the inbox ACPI-WMI driver,
the battery class driver, power APIs). No Dell DLLs, no direct hardware access. See [PLAN.md](PLAN.md).

Status: **Phase 1, read-only.** `myalien status` reads everything; nothing is written yet.

## Build

Requires Visual Studio 2022 (Desktop development with C++) — CMake ships with it.

```
cmake --preset vs2022
cmake --build --preset release
```

Output: `build/vs2022/src/cli/Release/myalien.exe` (self-contained, static CRT).
If `cmake` is not on `PATH`, use a "Developer PowerShell for VS 2022" or VS's bundled
`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

## Use

```
myalien status
```

Run from an **elevated** prompt to see the Dell thermal data and BIOS charge settings;
without elevation only Windows-side data (battery health, power source, power mode) is shown.

## Layout

| Path | Content |
|---|---|
| `src/core` | `myalien-core` static library: WMI wrapper, Dell AWCC thermal interface, Dell BIOS attributes, battery, system queries |
| `src/cli` | `myalien.exe` command line |
| `tools/discovery` | Phase 0 read-only discovery script |
| `docs/discovery` | Discovery dumps from this machine (reference data) |

## License

MIT — see [LICENSE](LICENSE). Linux drivers and other GPL projects were used as interface
documentation only; no code was copied from them.
