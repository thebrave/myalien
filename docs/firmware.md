# Firmware interfaces used by MyAlien

Reference for the interfaces MyAlien calls, and what was verified on a **Dell G15 5520**
(SKU `0B56`, BIOS 1.44.0) in November 2026. Interface facts come from the Linux kernel drivers
`alienware-wmi-wmax` and `dell-wmi-sysman`, and from tcc-g15; these were used as documentation
only, no code was taken from them.

All interfaces below are served by Windows' inbox ACPI-WMI driver (`wmiacpi.sys`) and battery class
driver. No Dell software or driver is required; Dell's `FusionService` (MyDell) may run alongside.

## 1. AWCC "WMAX" thermal interface

| | |
|---|---|
| Namespace / class | `ROOT\WMI` : `AWCCWmiMethodFunction` |
| GUID | `{A70591CE-A997-11DA-B012-B622A1EF5492}` |
| Instance | `ACPI\PNP0C14\AWCC_0` |
| Access | Administrator only (`WBEM_E_ACCESS_DENIED` otherwise) |

Each method takes one `uint32 arg2` and returns `uint32 argr`:

```
arg2 = op | arg1 << 8 | arg2 << 16 | arg3 << 24
argr = 0xFFFFFFFF or 0xFFFFFFFE  → failure / unsupported
```

### Operations MyAlien uses

| Method (ID) | Op | Args | Returns | Use |
|---|---|---|---|---|
| `Thermal_Information` (0x14) | `0x02` | – | bytes: fans, sensors, unknown, profiles | discovery |
| | `0x03` | index | resource ID (low byte) | discovery |
| | `0x04` | sensor ID | °C | temperature |
| | `0x05` | fan ID | rpm | fan speed |
| | `0x08` / `0x09` | fan ID | rpm | min / max speed |
| | `0x0B` | – | profile ID | current profile |
| | `0x0C` | fan ID | boost | current boost |
| `GetFanSensors` (0x13) | `0x01` | fan ID | count | sensors per fan |
| | `0x02` | fan ID, index | sensor ID | sensors per fan |
| `GameShiftStatus` (0x25) | `0x02` | – | 0 / non-zero | Game Shift status (read only) |
| `Thermal_Control` (0x15) | `0x01` | profile ID | – | **write**: activate profile |
| | `0x02` | fan ID, boost | – | **write**: set fan boost |

Never used: `GameShiftStatus` op `0x01` (toggle — see G-Mode below), overclocking, GPIO, Caldera,
Tobii and every other method of the class.

### What the G15 5520 reports

System description `0x04000202`: 2 fans, 2 sensors, 0 unknown, 4 profiles.

| Resource | ID | Notes |
|---|---|---|
| CPU fan | `0x32` | max 4800 rpm, min reported 0, follows sensor `0x01` |
| GPU fan | `0x33` | max 4800 rpm, min reported 0, follows sensor `0x06` |
| CPU sensor | `0x01` (raw `0x101`) | °C |
| GPU sensor | `0x06` (raw `0x106`) | °C |

Fan percentage (`Thermal_Information` op `0x06`) returns `0xFFFFFFFE` (unsupported). At idle the
fans regularly stop (0 rpm).

### Profiles

| ID | Name | Enumerated | `AWThermalManagement` shows | Verified write |
|---|---|---|---|---|
| `0xA0` | Balanced | yes | Balanced | yes |
| `0xA1` | Balanced-Performance | yes | Performance | yes |
| `0xA3` | Quiet | yes | Quiet | yes |
| `0xA5` | Low-Power | yes | Battery | starting state only |
| `0x00` | Custom | no | Balanced | yes |
| `0xAB` | G-Mode | no | Performance | yes |

This is the "USTT" profile family; the legacy `0x96–0x99` codes are not used by this machine.

### G-Mode

* Activate `0xAB` with `Thermal_Control` op `0x01`; leave it by activating a regular profile.
* The firmware then sets **Game Shift on and fan boost 100 on both fans** by itself, and clears
  both when G-Mode is left.
* The Linux G-series sequence (toggle Game Shift with `GameShiftStatus` op `0x01`, then activate
  `0xAB`) did **not** work here: the toggle left the status at 0. MyAlien never sends it.
* **Game Shift can get stuck** (2026-11-11): with G-Mode entered by MyAlien (`0xAB`), selecting Quiet
  in MyDell left the profile at `0xA3` but **Game Shift on and boost 100 on both fans** (fans at
  ~4800 rpm). Selecting a profile does not clear it; leaving G-Mode does (activate `0xAB`, then the
  profile). MyAlien detects this state ("G-Mode stuck"), `gmode off` / `profile set` repair it that
  way (verified: tray "G-Mode stuck" -> GMODE OFF 0xA3 -> Quiet, Game Shift off, fans back to
  ~600 rpm), and the monitor treats Game Shift as part of the setting.
* Reported by tcc-g15 users: a short system freeze when switching, and G-Mode kept for about a
  minute while hot.

### Fan boost

* Set with `Thermal_Control` op `0x02` (fan ID, boost); read with `Thermal_Information` op `0x0C`.
* Behaves as a **percentage of maximum speed**: boost 30 settled at ~1370–1420 rpm (≈ 29 % of
  4800). MyAlien caps it at 100 (the Linux driver accepts up to 255).
* Fans start reacting after 3–4 s and settle over several more seconds.
* Boost 0 in Custom: the firmware manages the fans (idle ~650–780 rpm or stopped).
* MyAlien only changes boost in the Custom profile.

## 2. Dell BIOS attributes

| | |
|---|---|
| Namespace | `ROOT\DCIM\SYSMAN\BIOSATTRIBUTES` |
| Read classes | `EnumerationAttribute`, `IntegerAttribute` (also `StringAttribute`) |
| Write | `BIOSAttributeInterface.SetAttribute(AttributeName, AttributeValue, SecHandle uint8[], SecHndCount, SecType) → Status` |
| Password state | `ROOT\DCIM\SYSMAN\WMISECURITY` : `PasswordObject` (`NameId = 'Admin'`, `IsPasswordSet`) |
| Access | Administrator only |

Without a BIOS password, `SetAttribute` works with `SecType = 0`, `SecHndCount = 0` and an empty
`SecHandle`. `Status` 0 = success.

### Attributes MyAlien uses

| Attribute | Type | Values on the 5520 | Access in MyAlien |
|---|---|---|---|
| `PrimaryBattChargeCfg` | enumeration | Adaptive, Standard, Express, PrimAcUse, Custom | read / write |
| `CustomChargeStart` | integer | 50–95, step 1 | read / write |
| `CustomChargeStop` | integer | 55–100, step 1 | read / write |
| `AdvBatteryChargeCfg` | enumeration | Disabled, Enabled | read only |
| `AWThermalManagement` | enumeration | Balanced, Battery, Quiet, Performance | read only (mirrors the WMAX profile, see table above) |

* `CustomChargeStart` / `CustomChargeStop` carry the modifier
  `[ReadOnlyIfNot:PrimaryBattChargeCfg=Custom]`.
* **Reads are slow**: any query on an attribute class (even `WHERE AttributeName = …`) makes the
  provider read every attribute of that class from the BIOS: ~1.5 s per class as SYSTEM, ~4.5 s
  for the three reads behind the charge settings. Read each class once, and cache.
* Charge-setting writes **apply immediately**: each value read back right after the write,
  no reboot needed.
* `AWThermalManagement` follows a WMAX profile write within 2 s. It cannot identify the profile
  on its own (Custom and Balanced both show "Balanced"; G-Mode and Balanced-Performance both
  show "Performance").

## 3. Battery class driver

`SetupDiGetClassDevs(GUID_DEVICE_BATTERY)` → `CreateFile` → `IOCTL_BATTERY_QUERY_TAG`, then
`IOCTL_BATTERY_QUERY_INFORMATION` (`BatteryInformation`, names, serial) and
`IOCTL_BATTERY_QUERY_STATUS`. No elevation needed.

On the 5520 (battery `DELL 70N2F32`, LiP): design 84 280 mWh, full charge 66 770 mWh (79.2 % health).
The firmware reports a cycle count of 0 and no temperature, so MyAlien hides both.
The WMI class `BatteryStaticData` fails even elevated, which is why the IOCTL path is used.

## 4. Other Windows APIs

| Need | API |
|---|---|
| Machine identity | `HKLM\HARDWARE\DESCRIPTION\System\BIOS` (`SystemManufacturer`, `SystemProductName`, `SystemSKU`, `BIOSVersion`) |
| Elevation | `GetTokenInformation(TokenElevation)` |
| MyDell backend | SCM: `OpenService("FusionService")` + `QueryServiceStatus` |
| Windows power mode | `PowerGetEffectiveOverlayScheme` (loaded from `powrprof.dll`) |
| AC / battery | `GetSystemPowerStatus` |

## 5. Not used, on purpose

* `ROOT\WMI : BFn.DoBFn` `{A80593CE-…}` — Dell SMBIOS / SMI calling interface.
* `dcdbas.sys`, `ROOT\Dell`, any Dell DLL.
* `MSAcpi_ThermalZoneTemperature` — "not supported" on this machine.
* `ROOT\WMI : BIOSEvent` `{9DBB5994-…}` — exists, but no events were seen for Fn keys or the
  power adapter.

## 6. Still to observe

* Whether a non-zero fan boost survives a profile change or resume from sleep.
* Whether the profile survives a **reboot** with `FusionService` stopped. (It survives **sleep**: Quiet
  was still active after a 33-minute sleep with FusionService stopped, 2026-11-11.)
* Whether MyDell's window refreshes by itself after MyAlien changes a setting.
