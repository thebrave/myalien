# MyAlien — plan

A lightweight replacement for MyDell / Alienware Command Center, scoped to the **Dell G15 5520**,
for **thermal control** and **battery care** only. No lighting, no overclocking, no telemetry, no Dell DLLs,
no direct EC/SMI/port I/O. Everything goes through firmware interfaces that Windows already exposes
(ACPI-WMI via the inbox `wmiacpi.sys` driver, plus the battery class driver and power APIs).

## 1. What this machine exposes (checked 2026-11-09)

| Item | Finding |
|---|---|
| Model / BIOS | `Dell G15 5520`, SKU `0B56`, BIOS **1.44.0** (08/26/2026) |
| AWCC "WMAX" interface | `root\wmi : AWCCWmiMethodFunction`, GUID `{A70591CE-A997-11DA-B012-B622A1EF5492}`, instance `ACPI\PNP0C14\AWCC_0` (inbox `wmiacpi.sys` / `wmiprov.dll`; no Dell driver needed). Methods include `Thermal_Information`, `Thermal_Control`, `GetFanSensors`, `GameShiftStatus`, `PowerInformation`, `SystemInformation`. Instance access **requires admin** (non-elevated reads fail with `WBEM_E_ACCESS_DENIED`). |
| Dell BIOS attributes over WMI | `root\dcim\sysman\biosattributes` : `EnumerationAttribute` (`{F1DDEE52-…-9BF5}`), `IntegerAttribute` (`…-9BFA`), `StringAttribute` (`…-9BF9`), `BIOSAttributeInterface` (`…-9BF4`, instance `ACPI\PNP0C14\WBAT_0`, `SetAttribute`). Served by `Provider_PlatformBios` (`{D2D588B5-…}` = inbox `wmiprov.dll` over `wmiacpi.sys`, no Dell DLL/driver). Instance access **requires admin**. Confirmed attributes in section 1b. |
| Battery (inbox) | `BatteryStatus`, `BatteryFullChargedCapacity`, `BatteryRuntime`, and `Win32_Battery` respond. `BatteryStaticData` fails even elevated, but `IOCTL_BATTERY_QUERY_INFORMATION` (`BatteryInformation`) succeeds non-elevated and reports `DesignedCapacity = 84280` mWh (`FullChargedCapacity = 66770` mWh → **79.2%** health, `Chemistry = LiP`). `CycleCount` is 0 in both WMI and IOCTL. `BatteryTemperature` returns no instances in WMI and `ERROR_INVALID_FUNCTION` (1) via IOCTL. |
| Thermal (inbox) | `MSAcpi_ThermalZoneTemperature` is registered but `Get-CimInstance` returns "not supported". Temps come from WMAX. Intel IPF (`Intel(R) Innovation Platform Framework Manager` + 6 `Generic Participant` devices, `EsifWmiEvent` in `root\wmi`) is present. |
| Other Dell ACPI-WMI | `BIOSEvent` `{9DBB5994-…}` (class exists; the watch recorded no events), `WMI_Query` `{8D9DDCBC-…}` (descriptor), `BFn.DoBFn` `{A80593CE-…}` (SMBIOS/SMI calling interface — **never used**). |
| Dell software | MyDell 2.2.6.0 (Appx) stays installed. `FusionService` was Running in the first two dumps and Stopped (start mode Auto) in the third. WMAX reads work in both states. Kernel driver `dcdbas.sys` is installed and Running (`root\Dell` namespace exists, `DellInstrumentation` driver Stopped); we do **not** use either. |

### 1b. Phase 0 results (confirmed, elevated, BIOS 1.44.0, SKU 0B56)

Dumps: `docs/discovery/discovery-mydell-running-20261109-200659.json`, `...-200741.json`, and `...-203752.json` (`FusionService` running),
and `docs/discovery/discovery-fusion-stopped-20261109-201722.json` (`FusionService` stopped, MyDell package still installed).

**WMAX reads do not need Fusion.** With the service stopped, the same instance, method list, system description, resource IDs, profile list, op `0x0A` = `0xA0`, and GameShift 0 still come back, including live RPM. That does not show that profile writes, re-apply, or a fan curve work standalone.

**`AWThermalManagement` tracked the live WMAX profile** in the four snapshots (all on AC):

| When | WMAX current profile | `AWThermalManagement` |
|---|---|---|
| 19:06:22Z, Fusion running | `0xA0` Balanced | Balanced |
| 19:07:04Z, Fusion running | `0xA5` Low-Power | Battery |
| 19:16:45Z, Fusion stopped | `0xA0` Balanced | Balanced |
| 19:37:15Z, Fusion running | `0xA0` Balanced | Balanced |

**Confirmed 2026-11-10 by `myalien profile set`** (elevated, Fusion running): writing `0xA3` → `Quiet`, `0xA1` → `Performance`, `0xA0` → `Balanced`, each read back within the 2 s poll. Full mapping: Balanced=`0xA0`, Performance=`0xA1`, Quiet=`0xA3`, Battery=`0xA5`. Kernel calls `0xA1` USTT Balanced-Performance; that is the spec name, not this machine's BIOS label. The return to Balanced happened in the same interval as stopping Fusion. The dumps cannot tell a mode change made before the stop from a reset caused by the stop, and they do not show that the BIOS attribute survives reboot. Later writes showed G-Mode (`0xAB`) reads as `Performance` and Custom (`0x00`) as `Balanced`, so the attribute cannot identify the profile on its own (see §7).

**WMAX** — instance `ACPI\PNP0C14\AWCC_0`. System description `0x04000202` → 2 fans, 2 sensors, 0 unknown, 4 profiles.

| Resource | ID (raw) | Notes |
|---|---|---|
| Fan CPU_1 | `0x32` | max 4800 rpm, min reported 0, linked sensor `0x01`. Fusion running: 2469 / 2226 rpm (hot/warm), 590 rpm (at 55°C). Fusion stopped: 879 rpm (at 55°C) |
| Fan GPU_1 | `0x33` | max 4800 rpm, min 0, linked sensor `0x06`. Fusion running: 2770 / 2501 rpm, 672 rpm (at 49°C). Fusion stopped: 416 rpm (at 50°C) |
| Sensor CPU | `0x01` (raw `0x0101`) | °C. Bit 8 is `AWCC_SENSOR_ID_FLAG`; the type is the low byte. Fusion running: 82°C, 50°C, 55°C. Fusion stopped: 55°C |
| Sensor GPU | `0x06` (raw `0x0106`) | °C, same flag. Fusion running: 44°C, 44°C, 49°C. Fusion stopped: 50°C |
| Profiles | `0xA0`, `0xA1`, `0xA5`, `0xA3` | **USTT** family. Kernel names: Balanced, Balanced-Performance, Low-Power, Quiet. No Cool (`0xA2`) and no USTT Performance (`0xA4`). G-Mode `0xAB` is not enumerated; `GameShiftStatus` answers (0 = off), which is the kernel's G-series signal. Custom `0x00` is not enumerated either. Both are untested writes. |

* Fan percentage (op 0x06) → `0xFFFFFFFE` (unsupported, `error = true`) on both fans, with and without Fusion. Fan boost reads 0. That is a current value of 0, not proof the op is unsupported. Kernel stores boost as a `u8` (0–255); tcc-g15 calls the same op an addon percent. Units stay unverified.
* Op 0x0A (balanced profile ID) → `0xA0` in every dump.
* The live profile changed between snapshots (see the table above), including once while Fusion was stopped. Hardware stays the source of truth. The 203752 dump (Fusion running, CPU 55°C / GPU 49°C, 590 / 672 rpm) confirms that the low RPM (~400–900 rpm) at ~50–55°C is the normal firmware Balanced fan curve, whether Fusion is running or stopped.

**BIOS attributes** (110 attributes). `PasswordObject` reports `IsPasswordSet=0` for Admin, System, and a third object with an empty name. The no-password `SetAttribute` buffer (`SecType=0`, `SecHndCount=0`, `SecHandle=[]`) was not observed, because the script never calls `SetAttribute`.

| Attribute | Current | Values |
|---|---|---|
| `PrimaryBattChargeCfg` | Adaptive | Adaptive, Standard, Express, PrimAcUse, Custom |
| `CustomChargeStart` | 50 (default 50) | 50..95, step 1. Modifier `[ReadOnlyIfNot:PrimaryBattChargeCfg=Custom]`. `ReadOnly` is still 0 while the mode is Adaptive |
| `CustomChargeStop` | 80 (default 90) | 55..100, step 1. Same modifier. Stop − start ≥ 5 (Dell / `dell-laptop.c`) |
| `AdvBatteryChargeCfg` | Disabled | Disabled, Enabled (Advanced Battery Charge mode — schedule-based, Dell-specific) |
| `AWThermalManagement` | see table above | Balanced, Battery, Quiet, Performance |
| `UsbPowerShare`, `WakeOnAc`, `TurboMode` | — | out of scope, read-only display at most |

Windows-side method: `BIOSAttributeInterface.SetAttribute(string AttributeName, string AttributeValue, uint8[] SecHandle, uint32 SecHndCount, uint32 SecType, out sint32 Status)` (+ `SetAttributes`, `SetDefaultValue`, `GetHelpString`). Note: `SetAttribute` (`WmiMethodId=4`) is an **instance** method on `BIOSAttributeInterface` (active instance `ACPI\PNP0C14\WBAT_0`, not `Static`), and `EnumerationAttribute` / `IntegerAttribute` expose `CurrentValue` only (no `PendingValue` property). In COM, pass a valid `VT_ARRAY | VT_UI1` `SAFEARRAY` for `SecHandle` even when `SecHndCount = 0`.

**Battery** — `Win32_Battery` name `DELL 70N2F32`, chemistry code 2 (Unknown), design voltage 12432–12445 mV. Full charge and remaining capacity 66770 mWh, on AC, estimated charge 100%, charge and discharge rate 0, in every dump. `BatteryStaticData` fails with a generic error even elevated and with Fusion stopped, but `IOCTL_BATTERY_QUERY_INFORMATION` (`BatteryInformation` on `\\?\acpi#pnp0c0a#1#{72631e54-78a4-11d0-bcf7-00aa00b7b32a}`) succeeds even non-elevated and returns `DesignedCapacity = 84280` mWh, `FullChargedCapacity = 66770` mWh (**79.2%** health), `Chemistry = LiP`, `Technology = 1`, `CycleCount = 0`. Do not display cycle count until a report shows a non-zero count. `BatteryTemperature` has no WMI instances and fails via IOCTL (`ERROR_INVALID_FUNCTION`). `BatteryRuntime` is `0xFFFFFFFF` while on AC.

**Not available**: `MSAcpi_ThermalZoneTemperature` (class present, instances unsupported) → temps come from WMAX only. `biosEvents` is JSON null in the first three dumps and `{"watchedSeconds": 30, "count": 0, "events": []}` in the fourth (`203752`). Note: the 5520's physical G-key is **F9** (`Fn+F9`); re-test whether pressing `Fn+F9` emits a `BIOSEvent` or keycode, and use Windows power notifications for adapter/power changes.

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
2. Select a thermal profile from the IDs the firmware enumerates (`0xA0`, `0xA1`, `0xA5`, `0xA3`). G-Mode `0xAB` and Custom `0x00` stay off the menu until a Phase 1 write shows they stick.
3. G-Mode on/off (tray menu item + optional global hotkey via `RegisterHotKey`; also check in Phase 1 how the physical **F9 / `Fn+F9` Game Shift key** on the 5520 surfaces when `FusionService` is stopped).
4. Custom mode: per-fan boost (manual), later an optional temperature→boost curve run by the service.
5. Optional: link thermal profile to Windows power mode (Best efficiency / Balanced / Best performance via `PowerGetEffectiveOverlayScheme` / `PowerSetActiveOverlayScheme`) and/or AC vs battery.
6. Re-apply chosen profile at boot and after resume (firmware may reset it).

**Battery care**
1. Charge mode: Standard / Express / Adaptive / Primarily AC / Custom (start %, stop %), via BIOS attributes WMI.
2. Health view: design capacity (via `IOCTL_BATTERY_QUERY_INFORMATION`; `BatteryStaticData` fails here) vs full-charge capacity 66770 mWh, plus charge/discharge rate from `BatteryStatus`. Do not show cycle count or a WMI battery temperature; this firmware does not report them.
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
* **Dependencies**: Windows SDK only — no third-party code. COM lifetime via the SDK's `Microsoft::WRL::ComPtr` plus a few small RAII helpers in `src/core/wmi.h` (`wil` was considered and dropped: not needed for this size). Settings: plain INI via `GetPrivateProfileString` unless something forces JSON.
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
| **0. Discovery** ✅ | `tools/discovery/Invoke-Discovery.ps1` → `docs/discovery/*.json`. Elevated read-only PowerShell dump: WMAX instance name, system description, all resource IDs, profiles, sensors/fans, current profile, G-Mode status, fan boosts; all BIOS attributes matching Batt/Charge/Therm/Fan; battery static data; WMI event classes in `root\wmi` + any `BIOSEvent` emitted by Fn shortcuts. Run once with MyDell running and once with `FusionService` stopped. Dumps stay local (git-ignored); the findings are summarised in §1b. | No |
| **1. Core + CLI** ✅ (commands done; a few observations left in §7) | CMake skeleton, `myalien-core` + `myalien.exe`: `status`, `profile get/set`, `gmode on/off`, `fan boost`, `battery status`, `battery mode set`. Manually verify each write against MyDell's display. **Done**: skeleton, core (WMI, WMAX reads, BIOS attribute reads, battery IOCTL, system/MyDell/power-mode), read-only `status`, `profile` / `profile set` (verified on the 5520: A5→A3→A1→A0→A3, firmware read-back OK each time), `gmode on/off` (verified, see §7), `battery` / `battery mode` (verified, applies immediately; planner unit-tested), `profile set custom` + `fan` / `fan boost` (verified, boost ≈ % of max rpm, capped at 100). **Remaining observations** (§7): MyDell window refresh, profile after reboot with Fusion stopped, whether a non-zero boost survives a profile change / resume. **Next**: Phase 2 (service). | Yes (manual, one at a time) |
| **2. Service** | `myalien-svc.exe`: install/uninstall, pipe protocol, MyDell detector, coexist/standalone policy, persistence, re-apply on boot/resume, conflict guard. | Yes |
| **3. Tray** | `myalien-tray.exe`: tray menu + status dialog, notifications on external changes. | via service |
| **4. Extras** | Custom fan curve, Windows power-mode linking, battery-care reminders, optional global G-Mode hotkey, installer (WiX MSI or a simple self-install). | via service |

## 7. Risks / unknowns

Resolved by Phase 0: profile list (USTT: A0/A1/A3/A5), charge attributes exist, `SetAttribute` signature, no BIOS password.

Still to verify in Phase 1 (first writes, one at a time, with MyDell open to cross-check):
* ~~Profile switch via `Thermal_Control` op 0x01~~ — verified for A0/A1/A3 (A5 was the starting state). Still open: does MyDell's window follow without reopening it?
* ~~G-Mode~~ — **verified 2026-11-10** (on/off twice, Quiet ↔ G-Mode). The Linux G-series sequence (GameShift toggle, then `0xAB`) failed on the 5520: the toggle (GameShiftStatus op 0x01) left the status at 0, so it was removed from the write allow-list. What works (as tcc-g15 does): `Thermal_Control` activate `0xAB`. Findings:
  * Current profile reads back `0xAB`; **Game Shift follows by itself** (on in G-Mode, off after leaving it) → it is a status, not something to drive.
  * `AWThermalManagement` = `Performance` in G-Mode — the same value as `0xA1`, so the BIOS attribute cannot tell G-Mode from Balanced-Performance; the WMAX profile is the authority.
  * Firmware sets **fan boost = 100 on both fans** in G-Mode (fans ~4000–4100 rpm at idle, max 4800); leaving G-Mode resets it. Strong hint that boost is a **percentage (0–100)**, not 0–255 — to confirm in the fan-boost step.
  * Still open: how MyDell reflects it; no freeze or "stuck in G-Mode" seen at idle temps (both reported by tcc-g15 users under load).
* ~~Custom `0x00` + fan boost~~ — **verified 2026-11-10** (Quiet → Custom, boost all 30, boost all 0, → Balanced). Findings:
  * `Thermal_Control` activate `0x00` is accepted although Custom is not enumerated; `AWThermalManagement` shows `Balanced` in Custom (same as `0xA0`).
  * Boost 30 settled at ~1370–1420 rpm ≈ 29 % of the 4800 rpm max → boost behaves as **percent of max speed**. MyAlien caps it at 100.
  * Fans react after ~3–4 s and settle over several more seconds, both up and down (`fan boost` now samples 8 s).
  * Boost 0 in Custom = firmware-managed: fans idled at ~650–780 rpm and also stopped (0 rpm) at ~55 °C.
  * Still open: whether a profile change or resume resets a **non-zero** boost (the test set boost 0 before leaving Custom, so it could not show it).
* ~~`SetAttribute` on the charge attributes~~ — **verified 2026-11-10** (Adaptive → Custom 55–85 → Custom 50–80 → Adaptive). Each write returned Status 0 and **read back immediately: no reboot needed**. No-password call works with `SecType=0, SecHndCount=0, SecHandle=[]`. The planner always switches to Custom before the thresholds, so whether the BIOS would reject a threshold write outside Custom was not exercised (not needed). Still open: whether MyDell's battery page follows.
* `AWThermalManagement`: mapping fully confirmed, and a WMAX write updates it within 2 s. Still open: does the profile survive a reboot with Fusion stopped? Never written by MyAlien (WMAX is the write path); read-only.
* If a BIOS admin password is set later, it must be prompted for each write, never stored.
* Safety: never send undocumented ops; never touch OC/GPIO/Caldera/Tobii/BFn methods.

## 8. Open decisions

* ~~UI framework~~ → decided: C++20, plain Win32 tray, no runtime dependency.
* ~~MyDell~~ → decided: must coexist with MyDell and work standalone (section 5).
* ~~License~~ → decided: MIT (see `LICENSE`); may move to GPL later. Consequence: GPL projects (tcc-g15, Linux drivers) are used as **specs only** — no code copied or adapted from them.
* Settings format: INI (zero deps) vs JSON (nlohmann/json).
* Code signing: unsigned is fine for personal use; SmartScreen will warn on other machines.
