#pragma once

// Small system queries that need no elevation: machine identity, elevation state,
// MyDell / FusionService presence, Windows power mode.

#include <optional>
#include <string>

namespace myalien {

struct MachineInfo {
    std::wstring manufacturer;
    std::wstring product;
    std::wstring sku;
    std::wstring biosVersion;

    bool isG15_5520() const;
};

MachineInfo machineInfo();

bool isElevated();

enum class ServiceState { NotInstalled, Stopped, Running, Transitioning, Unknown };
const char* toString(ServiceState s);

// Dell's MyDell backend service ("FusionService").
ServiceState fusionServiceState();

enum class PowerMode { BestEfficiency, Balanced, BestPerformance, Other, Unknown };
const char* toString(PowerMode m);

// Windows power mode slider (effective power overlay scheme).
PowerMode windowsPowerMode();

// GetSystemPowerStatus: AC present?
std::optional<bool> onAcPower();

}  // namespace myalien
