#pragma once

// Battery information through the inbox Windows battery class driver
// (SetupAPI + IOCTL_BATTERY_QUERY_*), the same source `powercfg /batteryreport` uses.
// Works without elevation.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myalien {

struct BatteryInfo {
    std::wstring deviceName;
    std::wstring manufacturer;
    std::wstring serial;
    std::string chemistry;

    bool relativeCapacity = false;  // capacities in % instead of mWh
    uint32_t designCapacity = 0;    // mWh
    uint32_t fullChargeCapacity = 0;
    std::optional<uint32_t> cycleCount;  // nullopt when firmware reports 0
    std::optional<double> temperatureC;

    // Live status
    std::optional<uint32_t> remainingCapacity;  // mWh
    std::optional<uint32_t> voltage_mV;
    std::optional<int32_t> rate_mW;  // >0 charging, <0 discharging
    bool onLine = false;
    bool charging = false;
    bool discharging = false;
    bool critical = false;

    std::optional<double> healthPercent() const;
    std::optional<double> chargePercent() const;
};

// One entry per battery present (the G15 5520 has one).
std::vector<BatteryInfo> queryBatteries();

}  // namespace myalien
