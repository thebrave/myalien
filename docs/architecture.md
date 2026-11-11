# Architecture

Phase 1 delivered a static core library and a command-line tool. Phase 2 (in progress) adds the
Windows service; the tray app (Phase 3, see [PLAN.md](../PLAN.md)) will reuse the same pieces.

```
myalien.exe (src/cli)  ──── \\.\pipe\MyAlien ────  myalien-svc.exe (src/svc, LocalSystem)
        │        (not elevated: STATUS via service)          │
        │                                                     │
        └──────────────────── myalien-core (src/core) ────────┘
                │            │             │               │
            WMI (COM)    WMI (COM)    battery IOCTL   SCM / registry / powrprof
          root\WMI    root\DCIM\SYSMAN    SetupAPI
          AWCC WMAX   BIOS attributes
```

## Build

* CMake ≥ 3.25, MSVC (VS 2022), C++20, x64. Preset `vs2022`, build presets `debug` / `release`.
* Static CRT (`/MT`): `myalien.exe` is a single self-contained file (~500 KB).
* `/W4 /permissive- /utf-8`; the tree builds without warnings.
* **No third-party code.** Only the Windows SDK: COM/WMI (`wbemuuid`, `ole32`, `oleaut32`),
  `setupapi`, `advapi32`; `powrprof.dll` is loaded at run time.
* Tests: `ctest --test-dir build/vs2022 -C Release` (charge planner, snapshot wire format).

## Core library — `src/core`

| File | Responsibility |
|---|---|
| `error.h/.cpp` | `HrError` (exception carrying an `HRESULT`, `accessDenied()`), readable WMI error texts, UTF-8 ⇄ UTF-16 helpers |
| `wmi.h/.cpp` | Thin RAII layer over COM WMI: `ComApartment` (COM init + security), `Bstr`, `ScopedVariant`, `Object` (typed property reads; one-element arrays read as scalars), `Namespace` (`connect`, `query`, `getClass`, `execMethod`) |
| `awcc.h/.cpp` | Dell AWCC "WMAX" thermal interface: capability discovery, profile, G-Mode, temperatures, fans, fan boost |
| `bios_attributes.h/.cpp` | Dell BIOS attributes: read enumeration / integer attributes, admin-password state, allow-listed `SetAttribute`; `expectedAwThermalManagement()` |
| `charge.h/.cpp` | Battery charge settings: `readChargeSettings()` and the pure planner `planChargeChange()` |
| `battery.h/.cpp` | Battery health and live status through the Windows battery class driver (`IOCTL_BATTERY_QUERY_*`), no elevation needed |
| `system.h/.cpp` | Machine identity (registry), elevation, `FusionService` state (SCM), Windows power mode (effective overlay scheme), AC/DC |
| `actions.h/.cpp` | Write operations shared by the elevated CLI and the service: `actSetProfile`, `actSetGMode`, `actSetFanBoost`, `actSetChargeMode` (validate, write, read back) returning output lines + exit code; profile names; pipe encoding of the output |
| `snapshot.h/.cpp` | `Snapshot`: everything `status` shows, collected with per-section errors (`collectSnapshot`), and its `key=value` wire format (`encodeSnapshot` / `decodeSnapshot`) |
| `ipc.h/.cpp` | Pipe client and protocol framing: `call(request)`, `OK` / `ERR` responses, server identity check |

### `Awcc`

Constructed once per command. The constructor connects to `ROOT\WMI`, finds the active
`AWCCWmiMethodFunction` instance and discovers the machine's capabilities:

* system description → counts of fans, sensors, unknown resources, profiles (each must be ≤ 16);
* resource IDs → fan IDs, sensor IDs (low byte; high byte = sensor type), profile IDs
  (G-Mode `0xAB` is never listed);
* for each fan, the temperature sensors it follows;
* whether `GameShiftStatus` answers (G-series machine → G-Mode available).

Every call goes through `invoke()` (one mutex, `GetMethod` → `SpawnInstance` → `Put("arg2")` →
`ExecMethod` → `Get("argr")`). `invoke()` is private and only reachable through:

* `read()` — refuses any (method, op) not on the **read allow-list**;
* `write()` — refuses any (method, op) not on the **write allow-list**.

Public writes:

| Method | Firmware call | Validation |
|---|---|---|
| `setProfile(id)` | `Thermal_Control` op `0x01` | `id` must be an enumerated profile or Custom `0x00`; G-Mode refused |
| `setGMode(on, profileWhenOff)` | `Thermal_Control` op `0x01` with `0xAB` / the profile | G-series only; the off-profile must be enumerated |
| `setFanBoost(fan, boost)` | `Thermal_Control` op `0x02` | fan must be enumerated; boost ≤ `kMaxFanBoost` (100) |

Return values `0xFFFFFFFF` / `0xFFFFFFFE` are treated as errors (`isAwccError`).

### `BiosAttributes`

Connects to `ROOT\DCIM\SYSMAN\BIOSATTRIBUTES`. Attribute names in WQL are checked to be
`[A-Za-z0-9_]` only. `set(name, value)`:

* only `PrimaryBattChargeCfg`, `CustomChargeStart`, `CustomChargeStop` are allowed;
* refuses when the BIOS admin password is set (read from `ROOT\DCIM\SYSMAN\WMISECURITY`);
* calls `BIOSAttributeInterface.SetAttribute` with `SecType = 0`, `SecHndCount = 0`, empty
  `SecHandle`, and throws on a non-zero `Status` (1 failed, 2 invalid parameter, 3 access denied,
  4 not supported, 5 memory, 6 protocol).

### `planChargeChange`

Pure function (no firmware access), unit-tested by `tests/charge_plan_test.cpp`. From the current
settings and the request it validates mode and range, then returns the ordered writes:

1. the mode, if it changes — first, because the thresholds are read-only unless the mode is Custom;
2. the thresholds, ordered so `stop − start ≥ 5` holds after each write: when the stop goes up
   (or stays), stop then start; when it goes down, start then stop. Unchanged values are skipped.

The test checks the validation cases and all 3 025 threshold transitions in 5 % steps.

## CLI — `src/cli`

| File | Command |
|---|---|
| `main.cpp` | Entry point, dispatch, help, top-level error handling (friendly access-denied message) |
| `common.h/.cpp` | Shared helpers: profile names ⇄ IDs, `describeProfile`, BIOS mirror read-back, `refuseWrite` (G15 5520 + elevation guard), `noteMyDell` |
| `status.cpp` | `status` (also exports the battery section for `battery`) |
| `profile.cpp` | `profile`, `profile set` |
| `gmode.cpp` | `gmode`, `gmode on/off` |
| `fan.cpp` | `fan`, `fan boost` |
| `battery.cpp` | `battery`, `battery mode` |

Every write command follows the same pattern:

1. parse and validate arguments (exit 2 on error);
2. `refuseWrite()` — machine must be a G15 5520 and the process elevated (exit 1);
3. check state preconditions (e.g. Custom profile for `fan boost`, G-Mode off for `profile set`);
4. print `Before`, skip if already in the requested state;
5. write, then **read back from the firmware** and print `After` / `reads …`;
6. exit 3 if the firmware does not report the requested state, else print `OK`.

The CLI holds no state between runs: the firmware is the source of truth.

## Service — `src/svc` (Phase 2)

| File | Responsibility |
|---|---|
| `main.cpp` | `myalien-svc [--install \| --uninstall \| --console]`; no argument = started by the SCM |
| `service.h/.cpp` | SCM integration (`ServiceMain`, control handler: stop, shutdown, power events), install / uninstall, console mode |
| `pipe_server.h/.cpp` | Named-pipe server and request dispatch |
| `context.h` | `ServiceContext`: events and state shared by all threads |
| `charge_cache.h/.cpp` | Cached BIOS charge settings and their refresher thread |
| `monitor.h/.cpp` | Thermal monitor: watches the firmware, remembers the MyAlien setting, re-applies it, conflict guard |
| `thermal_state.h/.cpp` | Thermal state read / comparison, `state.ini` persistence |
| `log.h/.cpp` | `<install dir>\logs\service.log` (rotated at 1 MB), or stderr in console mode |

**Install.** `--install` copies the executable to `%ProgramFiles%\MyAlien` (administrators-only
write access) and registers that copy: a LocalSystem service must not run from a user-writable
directory. Automatic start; restart after 5 s on the first two failures.

**Pipe.** `\\.\pipe\MyAlien`, one instance, message mode, overlapped I/O, every wait bounded
(3 s per I/O) and interruptible by the stop event.

* DACL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`: SYSTEM and Administrators full access,
  interactive users read/write; remote clients rejected (`PIPE_REJECT_REMOTE_CLIENTS`).
* Created with `FILE_FLAG_FIRST_PIPE_INSTANCE` and the handle is reused between clients, so no
  other process can take the name while the service runs.
* One request per connection; requests are capped at 4 KB, responses at 256 KB.

**Protocol.** Request = one UTF-8 line. Response = `OK` or `ERR <message>`, then a body.

| Request | Body of the `OK` response |
|---|---|
| `PING` | `service=MyAlien`, `version=…` |
| `STATUS` | an encoded `Snapshot` (`v=1`, then `key=value` lines): live thermal + battery, cached charge settings with `ch.age` |
| `STATUS FRESH` | same, after re-reading the charge settings (~5 s) |
| `PROFILE <name>` | encoded action output: `exit=N`, then one `o:` / `e:` line per output line |
| `GMODE ON` / `GMODE OFF <name>` | same |
| `FAN <cpu\|gpu\|all> <0-100>` | same |
| `CHARGE <mode> [<start> <stop>]` | same; the charge cache is updated from the values read back |

**Writes.** Any interactive user may send them (pipe DACL; decided 2026-11-11). Requests are
tokenised strictly (at most 5 tokens of at most 32 printable ASCII characters) and handed as text to
the same `act*` functions the elevated CLI calls: they validate every value against what the
firmware reports and refuse other machines. Each write is logged with the caller's
`DOMAIN\user` and PID (read by impersonating the client at identification level). Charge writes
hold the cache's refresh lock, so no background BIOS read interleaves with them.

**Charge-settings cache** (`charge_cache.h/.cpp`). Reading Dell BIOS attributes costs ~1.5 s per
attribute class on the G15 5520 (the provider reads every attribute from the BIOS), ~4.5 s for the
charge settings. The service keeps them in memory, refreshed by a background thread at start-up,
after resume and every 10 minutes, and on `STATUS FRESH` (used by `myalien battery`). Reads are
serialised; a request that waited for a running read reuses it. The BIOS mirror of the thermal
profile (`AWThermalManagement`) is not part of the service's answer. Measured: `myalien status`
through the service ≈ 0.25 s, `myalien battery` ≈ 4.8 s.

**Client side** (`ipc::call`): waits for the pipe, connects with `SECURITY_IDENTIFICATION` (the
server cannot impersonate the caller), checks that `GetNamedPipeServerProcessId` is the PID of the
running `MyAlien` service, then exchanges one message each way with timeouts. The check can be
bypassed only with `MYALIEN_TRUST_ANY_SERVER=1`, for `--console` development runs.

**CLI use.** Reads (`status`, `battery`, `profile`, `gmode`, `fan`) call `obtainSnapshot()`:
collect locally when elevated, else ask the service, else collect locally and show why the service
was not used. Writes go to the service first, even when elevated, so the service knows about every
change made through MyAlien; without the service they run in-process when elevated.

**Thermal monitor** (`monitor.h/.cpp`, PLAN.md §5). A thread with its own WMI connection:

* **Remembers** the setting made through MyAlien: after each successful `PROFILE` / `GMODE` / `FAN`
  request, the pipe thread reads the resulting profile and fan boosts from the firmware and records
  them in `<install dir>\state.ini` (administrators-only location). Boosts only count in Custom.
* **Watches** the firmware every 15 s (3 cheap AWCC calls). A change it did not record is an
  external change (MyDell, firmware, another tool): it is logged, and if it departs from the
  MyAlien setting that setting is marked *overridden*.
* **Re-applies** at start-up and 5 s after resume, if the firmware no longer reports the MyAlien
  setting, the setting is not overridden, MyDell's `FusionService` is not running (MyDell owns
  persistence when present) and the machine is the G15 5520. At most once per start-up / resume,
  with the result logged: no write loops.
* An override is cleared by the next change made through MyAlien.

**Power events** (suspend, resume, AC/DC, power-setting changes) are logged; resume wakes the
charge refresher and the thermal monitor. AC/DC rules are not implemented.

## Safety model, summarised

* Only documented firmware interfaces, through Windows' inbox ACPI-WMI and battery drivers.
  Never: Dell DLLs, `dcdbas`, SMI/`BFn.DoBFn`, EC or port I/O.
* Separate read and write allow-lists in the core; anything else throws before reaching WMI.
* Arguments validated against what the firmware itself reports (enumerated profiles and fans,
  BIOS attribute bounds), plus fixed caps (fan boost ≤ 100, 5 % charge window).
* Writes only on the target model and only elevated; each one verified by reading it back.
* The Game Shift toggle (`GameShiftStatus` op `0x01`) was tried once, had no visible effect on the
  5520, and was removed from the allow-list.
