// `myalien status` - read-only overview, printed from a Snapshot collected locally or by the service.

#include "cli/commands.h"
#include "cli/common.h"
#include "core/awcc.h"
#include "core/bios_attributes.h"
#include "core/error.h"
#include "core/snapshot.h"
#include "core/system.h"
#include "core/wmi.h"

#include <cstdio>
#include <format>
#include <optional>
#include <string>

namespace myalien::cli {

namespace {

void line(const std::string& s) {
    std::fputs(s.c_str(), stdout);
    std::fputc('\n', stdout);
}

void field(std::string_view label, const std::string& value) { line(std::format("  {:<14}: {}", label, value)); }

std::string unavailable(const SectionError& e) {
    if (e.accessDenied) return "unavailable (access denied - run from an elevated prompt or start the MyAlien service)";
    return "unavailable (" + e.message + ")";
}

template <typename T>
std::string optNum(const std::optional<T>& v) {
    return v ? std::to_string(*v) : std::string("n/a");
}

std::string mWh(uint32_t v) { return std::format("{:.1f} Wh", v / 1000.0); }

void printSystem(const Snapshot& s, std::string_view source) {
    line(std::format("MyAlien {} - {} {} (SKU {}, BIOS {})", MYALIEN_VERSION, s.manufacturer, s.product, s.sku,
                     s.biosVersion));
    if (!s.isG15_5520()) line("  warning: this is not a Dell G15 5520; values may be wrong.");
    field("Source", std::string(source));
    field("MyDell", std::format("FusionService {}", toString(s.fusion)));
    field("Power source", s.onAc ? (*s.onAc ? "AC" : "battery") : "unknown");
    field("Windows mode", toString(s.windowsMode));
}

void printThermal(const Snapshot& s) {
    line("");
    line("Thermal (Dell AWCC WMI)");
    if (s.thermalError) {
        field("Status", unavailable(s.thermalError));
        return;
    }

    std::string profileLine = std::format("{} (0x{:02X})", profileName(s.profile), s.profile);
    if (s.awThermal) profileLine += std::format("   [BIOS AWThermalManagement: {}]", *s.awThermal);
    field("Profile", profileLine);

    std::string avail;
    for (uint8_t p : s.profiles) avail += std::format("{}{} (0x{:02X})", avail.empty() ? "" : ", ", profileName(p), p);
    field("Available", avail.empty() ? "none" : avail);

    if (s.gameShiftSupported)
        field("G-Mode", std::format("{}   (Game Shift: {})", s.profile == profile::GMode ? "on" : "off",
                                    s.gameShift ? (*s.gameShift ? "on" : "off") : "n/a"));
    else
        field("G-Mode", "not supported");

    for (const auto& f : s.fans) {
        const std::string temp = f.tempC ? std::format("{} °C", *f.tempC) : "n/a";
        field(fanName(f.id), std::format("{:>6}   fan {} rpm (max {}), boost {}", temp, optNum(f.rpm),
                                         optNum(f.maxRpm), optNum(f.boost)));
    }
    for (const auto& sensor : s.extraSensors)
        if (sensor.tempC) field(sensorName(sensor.id), std::format("{} °C", *sensor.tempC));

    if (s.awThermal) {
        const wchar_t* hint = expectedAwThermalManagement(s.profile);
        if (hint && toUtf8(hint) != *s.awThermal)
            line("  note: AWThermalManagement does not mirror the active profile (unexpected, see docs/firmware.md).");
    }
}

void printBattery(const Snapshot& s) {
    line("");
    line("Battery");
    if (s.batteryError) field("Status", unavailable(s.batteryError));
    else if (s.batteries.empty()) field("Battery", "none found");

    for (const auto& b : s.batteries) {
        field("Model", std::format("{} ({}, {})", b.model, b.manufacturer, b.chemistry));
        if (const auto h = b.healthPercent())
            field("Health", std::format("{:.1f} %  ({} of {} design)", *h, mWh(b.fullChargeCapacity),
                                        mWh(b.designCapacity)));
        std::string state = b.charging ? "charging" : b.discharging ? "discharging" : "idle";
        if (b.onLine) state += ", on AC";
        if (b.critical) state += ", CRITICAL";
        if (const auto c = b.chargePercent()) field("Charge", std::format("{:.0f} %  ({})", *c, state));
        else field("Charge", state);
        if (b.rate_mW && *b.rate_mW != 0) field("Rate", std::format("{:+.1f} W", *b.rate_mW / 1000.0));
        if (b.cycleCount) field("Cycles", std::to_string(*b.cycleCount));
        if (b.temperatureC) field("Temperature", std::format("{:.1f} °C", *b.temperatureC));
    }

    if (s.chargeError) {
        field("Charge mode", unavailable(s.chargeError));
        return;
    }
    if (!s.chargeExposed) {
        field("Charge mode", "not exposed by this BIOS");
    } else {
        std::string v = s.chargeMode;
        if (s.chargeStart && s.chargeStop)
            v += std::format("   (custom range {}-{} %{})", *s.chargeStart, *s.chargeStop,
                             s.chargeMode == "Custom" ? "" : ", inactive");
        field("Charge mode", v);
        std::string opts;
        for (const auto& m : s.chargeModes) opts += (opts.empty() ? "" : ", ") + m;
        field("Modes", opts);
    }
    if (s.advancedChargeMode) field("Advanced mode", *s.advancedChargeMode);
    if (s.biosPasswordSet) field("BIOS password", *s.biosPasswordSet ? "admin password set (writes will need it)" : "none");
    if (s.chargeAgeSeconds) {
        const uint32_t a = *s.chargeAgeSeconds;
        field("Settings read", a < 60    ? std::format("{} s ago by the service", a)
                               : a < 3600 ? std::format("{} min ago by the service", a / 60)
                                          : std::format("{} h ago by the service", a / 3600));
    }
}

}  // namespace

void printBatterySection(const Snapshot& s) { printBattery(s); }

int cmdStatus(std::span<const std::wstring_view> /*args*/) {
    wmi::ComApartment com;
    const auto [snapshot, source] = obtainSnapshot({});
    printSystem(snapshot, source);
    printThermal(snapshot);
    printBattery(snapshot);
    return 0;
}

}  // namespace myalien::cli
