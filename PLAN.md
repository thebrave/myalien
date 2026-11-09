# MyAlien — plan

A lightweight replacement for MyDell / Alienware Command Center, scoped to the **Dell G15 5520**,
for **thermal control** and **battery care** only. No lighting, no overclocking, no telemetry, no Dell DLLs,
no direct EC/SMI/port I/O. Everything goes through firmware interfaces that Windows already exposes
(ACPI-WMI via the inbox `wmiacpi.sys` driver, plus the battery class driver and power APIs).

## 1. What this machine exposes (checked 2026-11-09)

| Item | Finding |
|---|---|
| Model / BIOS | `Dell G15 5520`, BIOS 1.43.0 |
| AWCC "WMAX" interface | `root\wmi : AWCCWmiMethodFunction`, GUID `{A70591CE-A997-11DA-B012-B622A1EF5492}`, served by the inbox Microsoft ACPI-WMI driver (no Dell driver needed). Methods include `Thermal_Information`, `Thermal_Control`, `GetFanSensors`, `GameShiftStatus`, `PowerInformation`, `SystemInformation`. Instance access **requires admin**. |
| Dell BIOS attributes over WMI | `root\dcim\sysman\biosattributes` : `EnumerationAttribute`, `IntegerAttribute`, `StringAttribute`, `BIOSAttributeInterface` (SetAttribute). Requires admin. Expected to carry `PrimaryBattChargeCfg`, `CustomChargeStart`, `CustomChargeStop` (not verified yet). |
| Battery (inbox) | `root\wmi : BatteryStaticData, BatteryStatus, BatteryFullChargedCapacity, BatteryCycleCount, BatteryTemperature, BatteryRuntime` + `Win32_Battery`. |
| Thermal (inbox) | `MSAcpi_ThermalZoneTemperature`, Intel IPF (Innovation Platform Framework) participants present. |
| Other Dell ACPI-WMI | `BIOSEvent` `{9DBB5994-…}` (hotkey / BIOS events — e.g. Fn shortcuts, adapter events), `WMI_Query` `{8D9DDCBC-…}` (descriptor), `BFn.DoBFn` `{A80593CE-…}` (SMBIOS/SMI calling interface — **never used**). |
| Dell software | MyDell 2.2.6 (Appx) + `FusionService` (running) — this is what currently drives thermal modes. `root\dcdbas` driver present (we will **not** use it). |

### 1b. Phase 0 results (confirmed, elevated, BIOS 1.44.0, SKU 0B56)

Dumps: `docs/discovery/discovery-mydell-running-20261109-*.json` (FusionService running) and
`docs/discovery/discovery-fusion-stopped-20261109-201722.json` (FusionService stopped).
**Everything below is identical with Fusion stopped**: same WMAX instance, resource IDs, profiles, readings, BIOS attributes.
The firmware interfaces do not depend on MyDell → standalone mode is viable.

**`AWThermalManagement` mirrors the active WMAX profile** (all 3 dumps):

| WMAX current profile | `AWThermalManagement` |
|---|---|
| `0xA0` Balanced | Balanced |
| `0xA5` Low-Power | Battery |
| `0xA3` Quiet | Quiet (inferred) |
| `0xA1` Balanced-Performance | Performance (inferred) |

So the firmware itself persists the last-activated profile in a BIOS setting, which very likely means it survives reboot
without any software. Consequence: the service probably does **not** need to re-apply the profile at boot — to verify in Phase 1
(set a profile, reboot with Fusion stopped, read it back). G-Mode and Custom/fan-boost are not covered by this attribute and may still need re-applying.

**WMAX** — instance `ACPI\PNP0C14\AWCC_0`. System description `0x04000202` → 2 fans, 2 sensors, 0 unknown, 4 profiles.

| Resource | ID (raw) | Notes |
|---|---|---|
| Fan CPU_1 | `0x32` | max 4800 rpm, min reported 0, linked sensor `0x01`; observed ~2200–2500 rpm |
| Fan GPU_1 | `0x33` | max 4800 rpm, min 0, linked sensor `0x06`; observed ~2500–2800 rpm |
| Sensor CPU | `0x01` (raw `0x101`) | °C; high byte = sensor type, use low byte |
| Sensor GPU | `0x06` (raw `0x106`) | °C |
| Profiles | `0xA0` Balanced, `0xA1` Balanced-Performance, `0xA5` Low-Power, `0xA3` Quiet | **USTT** family. G-Mode `0xAB` not enumerated but `GameShiftStatus` answers (0 = off) → G-series path applies. Custom `0x00` to be tested. |

* Fan percentage (op 0x06) → `0xFFFFFFFE` (unsupported). Fan boost reads 0 on both fans.
* Op 0x0A (balanced profile ID) → `0xA0`.
* Current profile changed between the two runs (`0xA0` → `0xA5`) while on AC with MyDell running: something (MyDell/Fusion or the user) changes it — confirms the need for the "hardware is the source of truth" polling.

**BIOS attributes** (110 attributes; no Admin/System password set → `SecType=0, SecHndCount=0, SecHandle=[]`):

| Attribute | Current | Values |
|---|---|---|
| `PrimaryBattChargeCfg` | Adaptive | Adaptive, Standard, Express, PrimAcUse, Custom |
| `CustomChargeStart` | 50 | 50..95 |
| `CustomChargeStop` | 80 | 55..100 |
| `AdvBatteryChargeCfg` | Disabled | Disabled, Enabled (Advanced Battery Charge mode — schedule-based, Dell-specific) |
| `AWThermalManagement` | Battery / Balanced | Balanced, Battery, Quiet, Performance — mirrors the active WMAX profile (see above) |
| `UsbPowerShare`, `WakeOnAc`, `TurboMode` | — | out of scope, read-only display at most |

Windows-side method: `BIOSAttributeInterface.SetAttribute(string AttributeName, string AttributeValue, uint8[] SecHandle, uint32 SecHndCount, uint32 SecType, out sint32 Status)` (+ `SetAttributes`, `SetDefaultValue`, `GetHelpString`).

**Battery** — `DELL 70N2F32`, SMP, LiPo, design 84 280 mWh, full charge 66 770 mWh → **health ≈ 79 %**. `BatteryStaticData` WMI fails even elevated and `CycleCount` = 0 (firmware doesn't report it) → use `IOCTL_BATTERY_QUERY_INFORMATION` (as `powercfg /batteryreport` does) for design capacity; don't show cycle count.

**Not available**: `MSAcpi_ThermalZoneTemperature` (unsupported) → temps come from WMAX only. No `BIOSEvent` captured during the 30 s watch → no hotkey/adapter events to rely on; use Windows power notifications instead.

## 2. Reference specs (open source)

* **Linux `alienware-wmi-wmax.c`** + docs `Documentation/wmi/devices/alienware-wmi.rst` — the best spec of the WMAX interface.
  The DMI table matches product name "Dell G15" with `g_series_quirks` (hwmon + platform_profile + G-Mode) → covers the 5520.
* **tcc-g15** (GPL-3.0, Python/PyQt, Windows) — Thermal Control Center for G15 5511–5530; proves the same WMI calls work from Windows user mode on this model.
* **alienfx-tools** (T-Troll) — broader Alienware suite, fan control via the same AWCC WMI.
* **Linux `dell-wmi-sysman`** — spec for Dell BIOS attribute WMI (GUIDs `F1DDEE52-063C-4784-A11E-8A06684B9BF{4,5,9,A}`), SetAttribute buffer, return codes.
* **Linux `dell-laptop.c`** — battery charge modes (Standard / Express / Adaptive / PrimAcUse / Custom) and custom threshold limits (start 50–95, stop 55–100, stop − start ≥ 5). We use the same semantics, but through BIOS-attribute WMI instead of SMBIOS tokens/SMI.

### WMAX encoding (summary)
Every method takes one `uint32 arg2` = `op | arg1<<8 | arg2<<16 | arg3<<24`, returns `uint32 argr`; `0xFFFFFFFF`/`0xFFFFFFFE` = error.

| Method (id) | op | Meaning |
|---|---|---|
| Thermal_Information (0x14) | 0x02 | system description: fans, temp sensors, unknown, profiles (1 byte each) |
| | 0x03 | resource ID at index (fans → sensors → unknown → profiles) |
| | 0x04 | temperature °C of sensor ID |
| | 0x05 / 0x08 / 0x09 | fan RPM / min RPM / max RPM |
| | 0x0B | current thermal profile |
| | 0x0C | fan boost of fan ID |
| Thermal_Control (0x15) | 0x01 | activate profile ID |
| | 0x02 | set fan boost (fan ID, 0–255) |
| GetFanSensors (0x13) | 0x01 / 0x02 | sensor count / sensor ID per fan |
| GameShiftStatus (0x25) | 0x01 / 0x02 | toggle / get G-Mode |

Profiles: Custom `0x00`, G-Mode `0xAB`, legacy Quiet/Balanced/BalPerf/Perf `0x96–0x99`, USTT Balanced `0xA0`, BalPerf `0xA1`, Cool `0xA2`, Quiet `0xA3`, Perf `0xA4`, LowPower `0xA5`.
Fan IDs: `0x32` CPU, `0x33` GPU. Sensor IDs: `0x01` CPU, `0x06` GPU.
G-Mode on G-series: toggle GameShift to match, then activate `0xAB` (Linux logic).

## 3. Scope (features)

**Thermal**
1. Show live CPU/GPU temps and fan RPM (+ min/max).
2. Select thermal profile among those the firmware enumerates (expected on the 5520: Balanced, Performance/G-Mode, Quiet, maybe Cool/Custom).
3. G-Mode on/off (software only — the 5520 has no G-key; a tray menu item and an optional global hotkey via `RegisterHotKey`).
4. Custom mode: per-fan boost (manual), later an optional temperature→boost curve run by the service.
5. Optional: link thermal profile to Windows power mode (Best efficiency / Balanced / Best performance via `PowerGetEffectiveOverlayScheme` / `PowerSetActiveOverlayScheme`) and/or AC vs battery.
6. Re-apply chosen profile at boot and after resume (firmware may reset it).

**Battery care**
1. Charge mode: Standard / Express / Adaptive / Primarily AC / Custom (start %, stop %), via BIOS attributes WMI.
2. Health view: design capacity vs full-charge capacity (wear %), cycle count, temperature, charge/discharge rate — inbox `root\wmi` battery classes or `IOCTL_BATTERY_QUERY_INFORMATION`.
3. Optional reminders (e.g. "on AC at 100% for N days → suggest Primarily AC / Custom 50–80").

**Out of scope**: lighting, OC, GPU MUX, updates, telemetry, anything outside these two areas.

## 4. Architecture

```
┌──────────────────────┐   named pipe (DACL: interactive users)   ┌───────────────────────────┐
│ myalien-tray.exe     │ ───────────────────────────────────────▶ │ myalien-svc.exe           │
│ plain Win32, as user │ ◀─────────────────────────────────────── │ LocalSystem Windows svc   │
└──────────────────────┘        state / telemetry / events        │  ├ AwccWmi   (root\wmi)   │
┌──────────────────────┐                                          │  ├ BiosAttr  (dcim)       │
│ myalien.exe (CLI)    │ ── same pipe, or direct (elevated) ────▶ │  ├ Battery   (inbox)      │
└──────────────────────┘                                          │  ├ MyDellDetector         │
                                                                  │  └ Policy engine          │
                                                                  └───────────────────────────┘
```

### Stack: C++20, Win32 only, no runtime dependency
* **Toolchain**: MSVC (VS 2022 Build Tools) + CMake, static CRT (`/MT`), x64. Target size: each exe < 1 MB; service working set ~2–3 MB.
* **Dependencies**: Windows SDK only, plus header-only [`wil`](https://github.com/microsoft/wil) (RAII for COM / handles / errors, MIT). JSON for settings: a small header-only lib (e.g. nlohmann/json, MIT) or a plain INI via `GetPrivateProfileString` — prefer INI-style keep-it-simple.
* **WMI**: COM `IWbemLocator` → `IWbemServices` (`ROOT\WMI`, `ROOT\DCIM\SYSMAN\BIOSATTRIBUTES`) → `ExecMethod`. One generic helper `CallWmax(method, op, a1, a2) -> uint32` wraps the boilerplate (`GetMethod` → `SpawnInstance` → `Put(L"arg2")` → `ExecMethod` → `Get(L"argr")`). WMI events (`ExecNotificationQueryAsync`) for battery / Dell hotkey events if useful.
* **Power / battery**: `RegisterPowerSettingNotification` / `SERVICE_CONTROL_POWEREVENT` (AC/DC, resume, battery %), `PowerGetEffectiveOverlayScheme` / `PowerSetActiveOverlayScheme` (Windows power mode), `IOCTL_BATTERY_QUERY_INFORMATION` / `IOCTL_BATTERY_QUERY_STATUS` via SetupAPI for battery health.
* **Service**: classic `StartServiceCtrlDispatcher` + `RegisterServiceCtrlHandlerEx`; self-install/uninstall via `myalien-svc.exe --install` (SCM APIs). Recovery actions set on install.
* **IPC**: one named pipe `\\.\pipe\MyAlien`, overlapped I/O, DACL `SYSTEM + Administrators: full; Interactive users: read/write`. Tiny length-prefixed message protocol (request/response + pushed state updates). Service validates every request (it is the only privileged component).
* **Tray (`myalien-tray.exe`)**: `Shell_NotifyIcon` + popup menu (profile radio items, G-Mode toggle, charge mode submenu) + one small dialog (`CreateDialog` from a .rc template) for temps/RPM/battery health, refreshed by a timer. Per-monitor DPI aware v2, light/dark aware icon. Started at logon via `HKCU\...\Run`. No admin rights needed.
* **CLI (`myalien.exe`)**: same commands as the tray; talks to the service if running, otherwise calls the core directly (requires elevation) — this is the Phase 1 test bench.
* **Core static lib `myalien-core`**: typed wrappers (`ThermalProfile`, `Fan`, `Sensor`, `ChargeMode`), capability discovery at startup (never hard-code what the firmware can enumerate), strict validation of every write (only known ops, boost 0–255, charge thresholds within limits). Serialises all WMAX calls through one mutex.

## 5. Coexistence with MyDell — and working without it

The app must behave correctly in both situations, decided at runtime, not at install time.

**Detection** (`MyDellDetector`, re-checked on service start and periodically / on SCM notification):
`FusionService` installed and running, and/or `DellInc.MyDell` package present. Exposed as a state: `Standalone` / `MyDellPresent`.

**Principle: hardware is the source of truth.** The service never assumes its last write is still in effect; it re-reads profile (op 0x0B), G-Mode (GameShift op 0x02) and fan boosts on a short poll (~2 s while the tray UI is open, ~15 s otherwise) and on resume / AC change. So changes made by MyDell, Fn shortcuts or the BIOS are always reflected in the UI.

| Behaviour | `MyDellPresent` (coexist) | `Standalone` |
|---|---|---|
| Show temps / RPM / profile / battery | yes | yes |
| Change profile / G-Mode on user action | yes (one-shot write, MyDell UI may lag) | yes |
| Re-apply saved profile at boot / resume | **no** — MyDell owns persistence | yes |
| AC/DC or power-mode → profile rules | off by default (would fight Fusion), opt-in with warning | yes |
| Custom fan curve (continuous boost writes) | off by default, opt-in with warning; auto-pauses if an external profile change is detected | yes |
| Global G-Mode hotkey (`RegisterHotKey`, optional) | off by default (MyDell may have its own) | available |
| Battery charge mode | yes (BIOS setting, persistent, last writer wins — same as MyDell) | yes |

**Conflict guard**: if the service writes a profile and within a few seconds reads back something else it did not write, it records "external change", stops any automatic re-apply for that session, and notifies the tray. No write loops, ever.

Stopping or uninstalling MyDell is the user's choice; the app never stops/disables `FusionService` itself.

## 6. Phases

| Phase | Deliverable | Writes to firmware? |
|---|---|---|
| **0. Discovery** ✅ | `tools/discovery/Invoke-Discovery.ps1` → `docs/discovery/*.json`. Elevated read-only PowerShell dump: WMAX instance name, system description, all resource IDs, profiles, sensors/fans, current profile, G-Mode status, fan boosts; all BIOS attributes matching Batt/Charge/Therm/Fan; battery static data; WMI event classes in `root\wmi` + any `BIOSEvent` emitted by Fn shortcuts. Run once with MyDell running and once with `FusionService` stopped. Commit the dump as the 5520 reference. | No |
| **1. Core + CLI** | CMake skeleton, `myalien-core` + `myalien.exe`: `status`, `profile get/set`, `gmode on/off`, `fan boost`, `battery status`, `battery mode set`. Manually verify each write against MyDell's display. | Yes (manual, one at a time) |
| **2. Service** | `myalien-svc.exe`: install/uninstall, pipe protocol, MyDell detector, coexist/standalone policy, persistence, re-apply on boot/resume, conflict guard. | Yes |
| **3. Tray** | `myalien-tray.exe`: tray menu + status dialog, notifications on external changes. | via service |
| **4. Extras** | Custom fan curve, Windows power-mode linking, battery-care reminders, optional global G-Mode hotkey, installer (WiX MSI or a simple self-install). | via service |

## 7. Risks / unknowns

Resolved by Phase 0: profile list (USTT: A0/A1/A3/A5), charge attributes exist, `SetAttribute` signature, no BIOS password.

Still to verify in Phase 1 (first writes, one at a time, with MyDell open to cross-check):
* Profile switch via `Thermal_Control` op 0x01 for each of A0/A1/A3/A5 — does MyDell follow?
* G-Mode: GameShift toggle + `0xAB` (Linux G-series sequence) — and how MyDell reflects it.
* Custom `0x00` + fan boost: meaning of the value (0–255 raw vs %), whether firmware resets it on profile change / resume.
* `SetAttribute` on `PrimaryBattChargeCfg` / `CustomChargeStart/Stop`: applies immediately or after reboot? Ordering rule when changing both thresholds (stop − start ≥ 5 must hold at every step).
* `AWThermalManagement`: confirm the Quiet/Performance mapping, that a WMAX profile change updates it immediately, and that the profile survives reboot with Fusion stopped. Never written by MyAlien (WMAX is the write path); read-only.
* If a BIOS admin password is set later, it must be prompted for each write, never stored.
* Safety: never send undocumented ops; never touch OC/GPIO/Caldera/Tobii/BFn methods.

## 8. Open decisions

* ~~UI framework~~ → decided: C++20, plain Win32 tray, no runtime dependency.
* ~~MyDell~~ → decided: must coexist with MyDell and work standalone (section 5).
* ~~License~~ → decided: MIT (see `LICENSE`); may move to GPL later. Consequence: GPL projects (tcc-g15, Linux drivers) are used as **specs only** — no code copied or adapted from them.
* Settings format: INI (zero deps) vs JSON (nlohmann/json).
* Code signing: unsigned is fine for personal use; SmartScreen will warn on other machines.
