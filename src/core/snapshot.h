#pragma once

// A point-in-time copy of everything `status` shows, independent of where it was collected:
// locally by the CLI, or by the service and sent over the pipe. Strings are UTF-8.
//
// Wire format (encodeSnapshot / decodeSnapshot): UTF-8 "key=value" lines, one per field,
// starting with "v=1". Lists use indexed keys (fan.0.id, fan.1.id, ...). Unknown keys are
// ignored so newer services stay readable by older clients.

#include "core/system.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myalien {

struct SnapshotFan {
    uint8_t id = 0;
    std::optional<uint32_t> rpm, maxRpm, boost;
    std::optional<int> tempC;  // first linked sensor
};

struct SnapshotSensor {  // sensors not linked to any fan
    uint8_t id = 0;
    std::optional<int> tempC;
};

struct SnapshotBattery {
    std::string model, manufacturer, chemistry;
    bool relativeCapacity = false;
    uint32_t designCapacity = 0, fullChargeCapacity = 0;  // mWh
    std::optional<uint32_t> remainingCapacity;            // mWh
    std::optional<int32_t> rate_mW;
    std::optional<uint32_t> cycleCount;
    std::optional<double> temperatureC;
    bool onLine = false, charging = false, discharging = false, critical = false;

    std::optional<double> healthPercent() const;
    std::optional<double> chargePercent() const;
};

// A section that could not be read: message + whether it was an access-denied error.
struct SectionError {
    std::string message;
    bool accessDenied = false;
    explicit operator bool() const noexcept { return !message.empty(); }
};

struct Snapshot {
    // System
    std::string manufacturer, product, sku, biosVersion;
    ServiceState fusion = ServiceState::Unknown;
    std::optional<bool> onAc;
    PowerMode windowsMode = PowerMode::Unknown;

    // Thermal (AWCC)
    SectionError thermalError;
    uint8_t profile = 0;
    std::vector<uint8_t> profiles;
    bool gameShiftSupported = false;
    std::optional<bool> gameShift;
    std::vector<SnapshotFan> fans;
    std::vector<SnapshotSensor> extraSensors;
    std::optional<std::string> awThermal;  // BIOS AWThermalManagement

    // Battery (inbox driver)
    SectionError batteryError;
    std::vector<SnapshotBattery> batteries;

    // Charge settings (BIOS attributes)
    SectionError chargeError;
    bool chargeExposed = false;
    std::string chargeMode;
    std::vector<std::string> chargeModes;
    std::optional<int64_t> chargeStart, chargeStop;
    std::optional<std::string> advancedChargeMode;
    std::optional<bool> biosPasswordSet;
    std::optional<uint32_t> chargeAgeSeconds;  // set when the charge settings come from the service cache

    bool isG15_5520() const { return manufacturer == "Dell Inc." && product == "Dell G15 5520"; }
};

struct SnapshotParts {
    bool thermal = true;
    bool battery = true;
    bool charge = true;
    // Read BIOS AWThermalManagement with the thermal part. Costs a full BIOS attribute read
    // (seconds on the G15 5520): the service skips it.
    bool biosMirror = true;
};

// Copies the charge-settings fields (charge*, advancedChargeMode, biosPasswordSet) from one
// snapshot to another.
void copyChargeSettings(const Snapshot& from, Snapshot& to);

// Diagnostics: time spent per section of collectSnapshot.
struct SnapshotTiming {
    double systemMs = 0, thermalMs = 0, batteryMs = 0, biosMs = 0;
    unsigned awccCalls = 0;
    double awccCallMs = 0;  // total time inside AWCC WMI method calls
    unsigned biosScans = 0;
    double biosScanMs = 0;  // total time reading BIOS attribute classes
};

// Collects a snapshot. Requires COM initialised on the calling thread (wmi::ComApartment).
// Never throws for firmware errors: they are recorded per section.
Snapshot collectSnapshot(const SnapshotParts& parts = {}, SnapshotTiming* timing = nullptr);

std::string encodeSnapshot(const Snapshot& s);

// Throws std::runtime_error on a malformed or unsupported payload.
Snapshot decodeSnapshot(std::string_view text);

}  // namespace myalien
