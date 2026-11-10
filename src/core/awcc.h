#pragma once

// Dell AWCC "WMAX" thermal interface: root\WMI : AWCCWmiMethodFunction,
// GUID {A70591CE-A997-11DA-B012-B622A1EF5492}, served by the inbox ACPI-WMI driver.
//
// Spec reference (interface facts only, no code reused): Linux
// Documentation/wmi/devices/alienware-wmi.rst and drivers/platform/x86/dell/alienware-wmi-wmax.c.
//
// Every method takes one uint32 `arg2` = op | arg1 << 8 | arg2 << 16 | arg3 << 24 and returns uint32 `argr`.
// 0xFFFFFFFF / 0xFFFFFFFE mean failure / unsupported.

#include "core/wmi.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace myalien {

enum class AwccMethod : uint8_t {
    GetFanSensors = 0x13,
    ThermalInformation = 0x14,
    ThermalControl = 0x15,
    GameShiftStatus = 0x25,
};

namespace profile {
inline constexpr uint8_t Custom = 0x00;
inline constexpr uint8_t GMode = 0xAB;
inline constexpr uint8_t QuietLegacy = 0x96;
inline constexpr uint8_t BalancedLegacy = 0x97;
inline constexpr uint8_t BalancedPerformanceLegacy = 0x98;
inline constexpr uint8_t PerformanceLegacy = 0x99;
inline constexpr uint8_t Balanced = 0xA0;
inline constexpr uint8_t BalancedPerformance = 0xA1;
inline constexpr uint8_t Cool = 0xA2;
inline constexpr uint8_t Quiet = 0xA3;
inline constexpr uint8_t Performance = 0xA4;
inline constexpr uint8_t LowPower = 0xA5;
}  // namespace profile

const char* profileName(uint8_t id);
const char* fanName(uint8_t id);
const char* sensorName(uint8_t id);

constexpr bool isAwccError(uint32_t v) noexcept { return v == 0xFFFFFFFFu || v == 0xFFFFFFFEu; }

struct AwccFan {
    uint8_t id = 0;
    std::vector<uint8_t> sensors;  // temperature sensors this fan follows
};

struct AwccSensor {
    uint8_t id = 0;    // low byte of the resource ID, used for reads
    uint8_t type = 0;  // high byte of the resource ID
};

struct AwccCapabilities {
    std::vector<AwccFan> fans;
    std::vector<AwccSensor> sensors;
    std::vector<uint8_t> profiles;  // as enumerated by firmware (G-Mode and Custom not listed)
    bool gameShift = false;         // GameShiftStatus answers -> G-series G-Mode path available
};

struct FanReading {
    uint8_t id = 0;
    std::optional<uint32_t> rpm, minRpm, maxRpm, boost;
};

class Awcc {
public:
    // Connects to root\WMI, finds the active AWCC instance and enumerates capabilities.
    // Throws HrError (access denied when not elevated) or std::runtime_error.
    Awcc();

    const AwccCapabilities& caps() const noexcept { return caps_; }
    const std::wstring& instancePath() const noexcept { return path_; }

    // --- reads (allow-listed operations only)
    uint8_t currentProfile();
    std::optional<int> temperatureC(uint8_t sensorId);
    FanReading fan(uint8_t fanId);
    std::optional<bool> gameShift();

    // --- writes (allow-listed operations only)
    // Activates one of the profiles the firmware enumerated (caps().profiles).
    // Custom (0x00) and G-Mode (0xAB) are refused here: they get dedicated commands.
    // Throws std::invalid_argument for a profile not offered by this machine,
    // std::runtime_error if the firmware rejects the call.
    void setProfile(uint8_t id);

    // Turns G-Mode on (activates profile 0xAB) or off (activates profileWhenOff, which must be an
    // offered profile). Only on G-series machines (Game Shift readable). Game Shift itself is
    // never written; its status is informational.
    void setGMode(bool on, uint8_t profileWhenOff = profile::Balanced);

private:
    void activate(uint8_t id);
    uint32_t read(AwccMethod m, uint8_t op, uint8_t a1 = 0, uint8_t a2 = 0);
    std::optional<uint32_t> tryRead(AwccMethod m, uint8_t op, uint8_t a1 = 0, uint8_t a2 = 0);
    uint32_t write(AwccMethod m, uint8_t op, uint8_t a1 = 0, uint8_t a2 = 0);
    uint32_t invoke(AwccMethod m, uint32_t arg);
    void discover();

    wmi::Namespace ns_;
    wmi::Object class_;
    std::wstring path_;
    AwccCapabilities caps_;
    std::mutex mutex_;
};

}  // namespace myalien
