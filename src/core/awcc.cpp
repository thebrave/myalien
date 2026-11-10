#include "core/awcc.h"

#include "core/error.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <stdexcept>

namespace myalien {

namespace {

// Thermal_Information ops
constexpr uint8_t kOpSystemDescription = 0x02;
constexpr uint8_t kOpResourceId = 0x03;
constexpr uint8_t kOpTemperature = 0x04;
constexpr uint8_t kOpFanRpm = 0x05;
constexpr uint8_t kOpFanMinRpm = 0x08;
constexpr uint8_t kOpFanMaxRpm = 0x09;
constexpr uint8_t kOpCurrentProfile = 0x0B;
constexpr uint8_t kOpFanBoost = 0x0C;
// GetFanSensors ops
constexpr uint8_t kOpFanSensorCount = 0x01;
constexpr uint8_t kOpFanSensorId = 0x02;
// GameShiftStatus ops
constexpr uint8_t kOpGameShiftGet = 0x02;

constexpr unsigned kMaxResources = 16;  // sanity bound per resource type

const wchar_t* methodName(AwccMethod m) {
    switch (m) {
        case AwccMethod::GetFanSensors: return L"GetFanSensors";
        case AwccMethod::ThermalInformation: return L"Thermal_Information";
        case AwccMethod::ThermalControl: return L"Thermal_Control";
        case AwccMethod::GameShiftStatus: return L"GameShiftStatus";
    }
    throw std::invalid_argument("unknown AWCC method");
}

// Read-only operations this module may send. Anything else is refused.
bool isAllowedRead(AwccMethod m, uint8_t op) {
    auto in = [op](std::initializer_list<uint8_t> ops) { return std::find(ops.begin(), ops.end(), op) != ops.end(); };
    switch (m) {
        case AwccMethod::ThermalInformation:
            return in({kOpSystemDescription, kOpResourceId, kOpTemperature, kOpFanRpm, kOpFanMinRpm, kOpFanMaxRpm,
                       kOpCurrentProfile, kOpFanBoost});
        case AwccMethod::GetFanSensors: return in({kOpFanSensorCount, kOpFanSensorId});
        case AwccMethod::GameShiftStatus: return in({kOpGameShiftGet});
        default: return false;
    }
}

constexpr uint32_t packArg(uint8_t op, uint8_t a1, uint8_t a2) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(a1) << 8) | (static_cast<uint32_t>(a2) << 16);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
const char* profileName(uint8_t id) {
    switch (id) {
        case profile::Custom: return "Custom";
        case profile::GMode: return "G-Mode";
        case profile::QuietLegacy: return "Quiet (legacy)";
        case profile::BalancedLegacy: return "Balanced (legacy)";
        case profile::BalancedPerformanceLegacy: return "Balanced-Performance (legacy)";
        case profile::PerformanceLegacy: return "Performance (legacy)";
        case profile::Balanced: return "Balanced";
        case profile::BalancedPerformance: return "Balanced-Performance";
        case profile::Cool: return "Cool";
        case profile::Quiet: return "Quiet";
        case profile::Performance: return "Performance";
        case profile::LowPower: return "Low-Power";
        default: return "Unknown";
    }
}

const char* fanName(uint8_t id) {
    switch (id) {
        case 0x32: return "CPU";
        case 0x33: return "GPU";
        case 0x34: return "PCI";
        case 0x35: return "Mid";
        case 0x36: return "Top 1";
        case 0x37: return "Side";
        case 0x3A: return "Front 1";
        case 0x3B: return "CPU 2";
        case 0x3C: return "GPU 2";
        default: return "Fan";
    }
}

const char* sensorName(uint8_t id) {
    switch (id) {
        case 0x01: return "CPU";
        case 0x03: return "Front";
        case 0x06: return "GPU";
        default: return "Sensor";
    }
}

// ---------------------------------------------------------------------------------------------
Awcc::Awcc() : ns_(wmi::Namespace::connect(L"ROOT\\WMI")) {
    for (const auto& o : ns_.query(L"SELECT * FROM AWCCWmiMethodFunction")) {
        if (o.getBool(L"Active").value_or(false)) {
            if (auto p = o.getString(L"__RELPATH")) {
                path_ = *p;
                break;
            }
        }
    }
    if (path_.empty()) throw std::runtime_error("No active AWCCWmiMethodFunction instance (AWCC WMI not available)");

    class_ = ns_.getClass(L"AWCCWmiMethodFunction");
    discover();
}

uint32_t Awcc::invoke(AwccMethod m, uint32_t arg) {
    std::lock_guard lock(mutex_);
    const wchar_t* name = methodName(m);

    wmi::ComPtr<IWbemClassObject> inDef;
    check(class_.get()->GetMethod(name, 0, &inDef, nullptr), "AWCC GetMethod");
    wmi::ComPtr<IWbemClassObject> in;
    check(inDef->SpawnInstance(0, &in), "AWCC SpawnInstance");

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;  // CIM uint32 is marshalled as VT_I4
    v.lVal = static_cast<LONG>(arg);
    check(in->Put(L"arg2", 0, &v, 0), "AWCC Put(arg2)");

    const auto out = ns_.execMethod(path_, name, in.Get());
    const auto r = out.getInt(L"argr");
    if (!r) throw std::runtime_error("AWCC: method returned no argr");
    return static_cast<uint32_t>(*r);
}

uint32_t Awcc::read(AwccMethod m, uint8_t op, uint8_t a1, uint8_t a2) {
    if (!isAllowedRead(m, op))
        throw std::logic_error(std::format("AWCC: refusing non allow-listed read (method 0x{:02X}, op 0x{:02X})",
                                           static_cast<unsigned>(m), op));
    return invoke(m, packArg(op, a1, a2));
}

std::optional<uint32_t> Awcc::tryRead(AwccMethod m, uint8_t op, uint8_t a1, uint8_t a2) {
    const uint32_t v = read(m, op, a1, a2);
    if (isAwccError(v)) return std::nullopt;
    return v;
}

void Awcc::discover() {
    const auto desc = tryRead(AwccMethod::ThermalInformation, kOpSystemDescription);
    if (!desc) throw std::runtime_error("AWCC: system description not available");

    const unsigned nFans = *desc & 0xFF;
    const unsigned nSensors = (*desc >> 8) & 0xFF;
    const unsigned nUnknown = (*desc >> 16) & 0xFF;
    const unsigned nProfiles = (*desc >> 24) & 0xFF;
    if (nFans > kMaxResources || nSensors > kMaxResources || nUnknown > kMaxResources || nProfiles > kMaxResources)
        throw std::runtime_error(std::format("AWCC: implausible system description 0x{:08X}", *desc));

    const unsigned total = nFans + nSensors + nUnknown + nProfiles;
    for (unsigned i = 0; i < total; ++i) {
        const auto r = tryRead(AwccMethod::ThermalInformation, kOpResourceId, static_cast<uint8_t>(i));
        if (!r) {
            if (i >= nFans + nSensors + nUnknown) break;  // some firmware over-reports profiles
            throw std::runtime_error(std::format("AWCC: resource {} not readable", i));
        }
        const uint8_t id = static_cast<uint8_t>(*r & 0xFF);
        if (i < nFans) {
            caps_.fans.push_back({id, {}});
        } else if (i < nFans + nSensors) {
            caps_.sensors.push_back({id, static_cast<uint8_t>((*r >> 8) & 0xFF)});
        } else if (i < nFans + nSensors + nUnknown) {
            // unknown resource type: ignored
        } else if (id != profile::GMode) {
            caps_.profiles.push_back(id);
        }
    }

    for (auto& f : caps_.fans) {
        const auto n = tryRead(AwccMethod::GetFanSensors, kOpFanSensorCount, f.id);
        if (!n || *n > kMaxResources) continue;
        for (uint32_t j = 0; j < *n; ++j) {
            if (const auto s = tryRead(AwccMethod::GetFanSensors, kOpFanSensorId, f.id, static_cast<uint8_t>(j)))
                f.sensors.push_back(static_cast<uint8_t>(*s & 0xFF));
        }
    }

    caps_.gameShift = tryRead(AwccMethod::GameShiftStatus, kOpGameShiftGet).has_value();
}

// ---------------------------------------------------------------------------------------------
uint8_t Awcc::currentProfile() {
    const auto v = tryRead(AwccMethod::ThermalInformation, kOpCurrentProfile);
    if (!v) throw std::runtime_error("AWCC: current profile not available");
    return static_cast<uint8_t>(*v & 0xFF);
}

std::optional<int> Awcc::temperatureC(uint8_t sensorId) {
    const auto v = tryRead(AwccMethod::ThermalInformation, kOpTemperature, sensorId);
    if (!v) return std::nullopt;
    return static_cast<int>(*v);
}

FanReading Awcc::fan(uint8_t fanId) {
    FanReading r;
    r.id = fanId;
    r.rpm = tryRead(AwccMethod::ThermalInformation, kOpFanRpm, fanId);
    r.minRpm = tryRead(AwccMethod::ThermalInformation, kOpFanMinRpm, fanId);
    r.maxRpm = tryRead(AwccMethod::ThermalInformation, kOpFanMaxRpm, fanId);
    r.boost = tryRead(AwccMethod::ThermalInformation, kOpFanBoost, fanId);
    return r;
}

std::optional<bool> Awcc::gameShift() {
    if (!caps_.gameShift) return std::nullopt;
    const auto v = tryRead(AwccMethod::GameShiftStatus, kOpGameShiftGet);
    if (!v) return std::nullopt;
    return *v != 0;
}

}  // namespace myalien
