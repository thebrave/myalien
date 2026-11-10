// `myalien status` - read-only overview.

#include "cli/commands.h"
#include "core/awcc.h"
#include "core/battery.h"
#include "core/bios_attributes.h"
#include "core/error.h"
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

std::string unavailable(const std::exception& e) {
    if (const auto* hr = dynamic_cast<const HrError*>(&e); hr && hr->accessDenied())
        return "unavailable (access denied - run from an elevated prompt)";
    return std::string("unavailable (") + e.what() + ")";
}

std::string optNum(const std::optional<uint32_t>& v, std::string_view unit = "") {
    return v ? std::format("{}{}", *v, unit) : std::string("n/a");
}

std::string mWh(uint32_t v) { return std::format("{:.1f} Wh", v / 1000.0); }

// ---------------------------------------------------------------------------------------------
void printSystem() {
    const auto m = machineInfo();
    line(std::format("MyAlien {} - {} {} (SKU {}, BIOS {})", MYALIEN_VERSION, toUtf8(m.manufacturer), toUtf8(m.product),
                     toUtf8(m.sku), toUtf8(m.biosVersion)));
    if (!m.isG15_5520()) line("  warning: this is not a Dell G15 5520; values may be wrong.");
    field("Elevated", isElevated() ? "yes" : "no");
    field("MyDell", std::format("FusionService {}", toString(fusionServiceState())));
    const auto ac = onAcPower();
    field("Power source", ac ? (*ac ? "AC" : "battery") : "unknown");
    field("Windows mode", toString(windowsPowerMode()));
}

void printThermal(std::optional<std::wstring>& awThermal) {
    line("");
    line("Thermal (Dell AWCC WMI)");
    try {
        Awcc awcc;
        const auto& caps = awcc.caps();

        const uint8_t cur = awcc.currentProfile();
        std::string profileLine = std::format("{} (0x{:02X})", profileName(cur), cur);
        if (awThermal) profileLine += std::format("   [BIOS AWThermalManagement: {}]", toUtf8(*awThermal));
        field("Profile", profileLine);

        std::string avail;
        for (uint8_t p : caps.profiles) avail += std::format("{}{} (0x{:02X})", avail.empty() ? "" : ", ", profileName(p), p);
        field("Available", avail.empty() ? "none" : avail);

        if (const auto gs = awcc.gameShift()) field("G-Mode", *gs ? "on" : "off");
        else field("G-Mode", "not supported");

        for (const auto& f : caps.fans) {
            const auto r = awcc.fan(f.id);
            std::string temp = "n/a";
            if (!f.sensors.empty())
                if (const auto t = awcc.temperatureC(f.sensors.front())) temp = std::format("{} °C", *t);
            field(fanName(f.id), std::format("{:>6}   fan {} rpm (max {}), boost {}", temp, optNum(r.rpm),
                                             optNum(r.maxRpm), optNum(r.boost)));
        }

        // Sensors not attached to any fan.
        for (const auto& s : caps.sensors) {
            bool attached = false;
            for (const auto& f : caps.fans)
                for (uint8_t id : f.sensors) attached |= (id == s.id);
            if (!attached)
                if (const auto t = awcc.temperatureC(s.id)) field(sensorName(s.id), std::format("{} °C", *t));
        }

        if (awThermal) {
            const wchar_t* hint = expectedAwThermalManagement(cur);
            if (hint && hint != *awThermal)
                line("  note: AWThermalManagement does not mirror the active profile (unexpected, see PLAN.md 1b).");
        }
    } catch (const std::exception& e) {
        field("Status", unavailable(e));
    }
}

void printBattery(const BiosAttributes* bios, const std::string& biosError) {
    line("");
    line("Battery");
    try {
        const auto batteries = queryBatteries();
        if (batteries.empty()) field("Battery", "none found");
        for (const auto& b : batteries) {
            field("Model", std::format("{} ({}, {})", toUtf8(b.deviceName), toUtf8(b.manufacturer), b.chemistry));
            if (const auto h = b.healthPercent())
                field("Health", std::format("{:.1f} %  ({} of {} design)", *h, mWh(b.fullChargeCapacity),
                                            mWh(b.designCapacity)));
            std::string state = b.charging ? "charging" : b.discharging ? "discharging" : "idle";
            if (b.onLine) state += ", on AC";
            if (b.critical) state += ", CRITICAL";
            if (const auto c = b.chargePercent())
                field("Charge", std::format("{:.0f} %  ({})", *c, state));
            else
                field("Charge", state);
            if (b.rate_mW && *b.rate_mW != 0) field("Rate", std::format("{:+.1f} W", *b.rate_mW / 1000.0));
            if (b.cycleCount) field("Cycles", std::to_string(*b.cycleCount));
            if (b.temperatureC) field("Temperature", std::format("{:.1f} °C", *b.temperatureC));
        }
    } catch (const std::exception& e) {
        field("Status", unavailable(e));
    }

    if (!bios) {
        field("Charge mode", biosError);
        return;
    }
    try {
        const auto mode = bios->enumeration(attr::PrimaryBattChargeCfg);
        if (!mode) {
            field("Charge mode", "not exposed by this BIOS");
        } else {
            std::string v = toUtf8(mode->current);
            const auto start = bios->integer(attr::CustomChargeStart);
            const auto stop = bios->integer(attr::CustomChargeStop);
            if (start && stop)
                v += std::format("   (custom range {}-{} %{})", start->current, stop->current,
                                 mode->current == L"Custom" ? "" : ", inactive");
            field("Charge mode", v);
            std::string opts;
            for (const auto& p : mode->possible) opts += (opts.empty() ? "" : ", ") + toUtf8(p);
            field("Modes", opts);
        }
        if (const auto adv = bios->enumeration(attr::AdvBatteryChargeCfg))
            field("Advanced mode", toUtf8(adv->current));
        if (const auto pw = bios->adminPasswordSet())
            field("BIOS password", *pw ? "admin password set (writes will need it)" : "none");
    } catch (const std::exception& e) {
        field("Charge mode", unavailable(e));
    }
}

}  // namespace

int cmdStatus(std::span<const std::wstring_view> /*args*/) {
    wmi::ComApartment com;

    printSystem();

    // BIOS attributes are read once and shared by the thermal (AWThermalManagement) and battery sections.
    std::optional<BiosAttributes> bios;
    std::string biosError;
    std::optional<std::wstring> awThermal;
    try {
        bios.emplace();
        if (const auto a = bios->enumeration(attr::AWThermalManagement)) awThermal = a->current;
    } catch (const std::exception& e) {
        biosError = unavailable(e);
        bios.reset();
    }

    printThermal(awThermal);
    printBattery(bios ? &*bios : nullptr, biosError);
    return 0;
}

}  // namespace myalien::cli
