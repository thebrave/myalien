#include "core/snapshot.h"

#include "core/awcc.h"
#include "core/battery.h"
#include "core/bios_attributes.h"
#include "core/error.h"

#include <charconv>
#include <format>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace myalien {

// ---------------------------------------------------------------------------------------------
std::optional<double> SnapshotBattery::healthPercent() const {
    if (relativeCapacity || designCapacity == 0 || fullChargeCapacity == 0) return std::nullopt;
    return 100.0 * fullChargeCapacity / designCapacity;
}

std::optional<double> SnapshotBattery::chargePercent() const {
    if (!remainingCapacity || fullChargeCapacity == 0) return std::nullopt;
    return 100.0 * *remainingCapacity / fullChargeCapacity;
}

// ---------------------------------------------------------------------------------------------
namespace {

SectionError toSectionError(const std::exception& e) {
    const auto* hr = dynamic_cast<const HrError*>(&e);
    return SectionError{e.what(), hr && hr->accessDenied()};
}

void collectThermal(Snapshot& s) {
    try {
        Awcc awcc;
        const auto& caps = awcc.caps();
        s.profile = awcc.currentProfile();
        s.profiles = caps.profiles;
        s.gameShiftSupported = caps.gameShift;
        if (caps.gameShift) s.gameShift = awcc.gameShift();

        for (const auto& f : caps.fans) {
            const auto r = awcc.fan(f.id);
            SnapshotFan sf;
            sf.id = f.id;
            sf.rpm = r.rpm;
            sf.maxRpm = r.maxRpm;
            sf.boost = r.boost;
            if (!f.sensors.empty()) sf.tempC = awcc.temperatureC(f.sensors.front());
            s.fans.push_back(sf);
        }
        for (const auto& sensor : caps.sensors) {
            bool linked = false;
            for (const auto& f : caps.fans)
                for (uint8_t id : f.sensors) linked |= (id == sensor.id);
            if (!linked) s.extraSensors.push_back({sensor.id, awcc.temperatureC(sensor.id)});
        }
    } catch (const std::exception& e) {
        s.thermalError = toSectionError(e);
    }
}

void collectBattery(Snapshot& s) {
    try {
        for (const auto& b : queryBatteries()) {
            SnapshotBattery sb;
            sb.model = toUtf8(b.deviceName);
            sb.manufacturer = toUtf8(b.manufacturer);
            sb.chemistry = b.chemistry;
            sb.relativeCapacity = b.relativeCapacity;
            sb.designCapacity = b.designCapacity;
            sb.fullChargeCapacity = b.fullChargeCapacity;
            sb.remainingCapacity = b.remainingCapacity;
            sb.rate_mW = b.rate_mW;
            sb.cycleCount = b.cycleCount;
            sb.temperatureC = b.temperatureC;
            sb.onLine = b.onLine;
            sb.charging = b.charging;
            sb.discharging = b.discharging;
            sb.critical = b.critical;
            s.batteries.push_back(std::move(sb));
        }
    } catch (const std::exception& e) {
        s.batteryError = toSectionError(e);
    }
}

void collectBios(Snapshot& s, bool thermal, bool charge) {
    try {
        BiosAttributes bios;
        if (thermal)
            if (const auto a = bios.enumeration(attr::AWThermalManagement)) s.awThermal = toUtf8(a->current);
        if (!charge) return;

        if (const auto mode = bios.enumeration(attr::PrimaryBattChargeCfg)) {
            s.chargeExposed = true;
            s.chargeMode = toUtf8(mode->current);
            for (const auto& p : mode->possible) s.chargeModes.push_back(toUtf8(p));
            if (const auto a = bios.integer(attr::CustomChargeStart)) s.chargeStart = a->current;
            if (const auto a = bios.integer(attr::CustomChargeStop)) s.chargeStop = a->current;
        }
        if (const auto adv = bios.enumeration(attr::AdvBatteryChargeCfg)) s.advancedChargeMode = toUtf8(adv->current);
        s.biosPasswordSet = bios.adminPasswordSet();
    } catch (const std::exception& e) {
        if (charge) s.chargeError = toSectionError(e);
    }
}

}  // namespace

Snapshot collectSnapshot(const SnapshotParts& parts) {
    Snapshot s;
    const auto m = machineInfo();
    s.manufacturer = toUtf8(m.manufacturer);
    s.product = toUtf8(m.product);
    s.sku = toUtf8(m.sku);
    s.biosVersion = toUtf8(m.biosVersion);
    s.fusion = fusionServiceState();
    s.onAc = onAcPower();
    s.windowsMode = windowsPowerMode();

    if (parts.thermal) collectThermal(s);
    if (parts.battery) collectBattery(s);
    if (parts.thermal || parts.charge) collectBios(s, parts.thermal, parts.charge);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Encoding
namespace {

class Writer {
public:
    void put(std::string_view key, std::string_view value) {
        out_ += key;
        out_ += '=';
        for (char c : value) out_ += (c == '\r' || c == '\n') ? ' ' : c;
        out_ += '\n';
    }
    template <typename T>
    void put(std::string_view key, const std::optional<T>& v) {
        if (v) putValue(key, *v);
    }
    void putValue(std::string_view key, bool v) { put(key, v ? "1" : "0"); }
    void putValue(std::string_view key, double v) { put(key, std::format("{:.2f}", v)); }
    void putValue(std::string_view key, const std::string& v) { put(key, v); }
    template <typename T>
        requires std::is_integral_v<T>
    void putValue(std::string_view key, T v) {
        put(key, std::to_string(v));
    }
    void putError(std::string_view prefix, const SectionError& e) {
        if (!e) return;
        put(std::format("{}.error", prefix), e.message);
        putValue(std::format("{}.denied", prefix), e.accessDenied);
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

std::string joinBytes(const std::vector<uint8_t>& v) {
    std::string r;
    for (uint8_t b : v) r += (r.empty() ? "" : ",") + std::to_string(b);
    return r;
}

std::string joinStrings(const std::vector<std::string>& v) {
    std::string r;
    for (const auto& s : v) r += (r.empty() ? "" : ",") + s;
    return r;
}

}  // namespace

std::string encodeSnapshot(const Snapshot& s) {
    Writer w;
    w.put("v", "1");
    w.put("sys.manufacturer", s.manufacturer);
    w.put("sys.product", s.product);
    w.put("sys.sku", s.sku);
    w.put("sys.bios", s.biosVersion);
    w.putValue("sys.fusion", static_cast<int>(s.fusion));
    w.put("sys.ac", s.onAc);
    w.putValue("sys.mode", static_cast<int>(s.windowsMode));

    w.putError("th", s.thermalError);
    if (!s.thermalError) {
        w.putValue("th.profile", s.profile);
        w.put("th.profiles", joinBytes(s.profiles));
        w.putValue("th.gs.supported", s.gameShiftSupported);
        w.put("th.gs", s.gameShift);
        for (size_t i = 0; i < s.fans.size(); ++i) {
            const auto& f = s.fans[i];
            const auto k = std::format("fan.{}.", i);
            w.putValue(k + "id", f.id);
            w.put(k + "rpm", f.rpm);
            w.put(k + "max", f.maxRpm);
            w.put(k + "boost", f.boost);
            w.put(k + "temp", f.tempC);
        }
        for (size_t i = 0; i < s.extraSensors.size(); ++i) {
            const auto k = std::format("sensor.{}.", i);
            w.putValue(k + "id", s.extraSensors[i].id);
            w.put(k + "temp", s.extraSensors[i].tempC);
        }
    }
    w.put("th.aw", s.awThermal);

    w.putError("bat", s.batteryError);
    for (size_t i = 0; i < s.batteries.size(); ++i) {
        const auto& b = s.batteries[i];
        const auto k = std::format("bat.{}.", i);
        w.put(k + "model", b.model);
        w.put(k + "manufacturer", b.manufacturer);
        w.put(k + "chemistry", b.chemistry);
        w.putValue(k + "relative", b.relativeCapacity);
        w.putValue(k + "design", b.designCapacity);
        w.putValue(k + "full", b.fullChargeCapacity);
        w.put(k + "remaining", b.remainingCapacity);
        w.put(k + "rate", b.rate_mW);
        w.put(k + "cycles", b.cycleCount);
        w.put(k + "temp", b.temperatureC);
        w.putValue(k + "online", b.onLine);
        w.putValue(k + "charging", b.charging);
        w.putValue(k + "discharging", b.discharging);
        w.putValue(k + "critical", b.critical);
    }

    w.putError("ch", s.chargeError);
    w.putValue("ch.exposed", s.chargeExposed);
    if (s.chargeExposed) {
        w.put("ch.mode", s.chargeMode);
        w.put("ch.modes", joinStrings(s.chargeModes));
        w.put("ch.start", s.chargeStart);
        w.put("ch.stop", s.chargeStop);
    }
    w.put("ch.adv", s.advancedChargeMode);
    w.put("ch.pw", s.biosPasswordSet);
    return w.take();
}

// ---------------------------------------------------------------------------------------------
// Decoding
namespace {

class Reader {
public:
    explicit Reader(std::string_view text) {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            pos = end + 1;
            if (line.empty()) continue;
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos || eq == 0) throw std::runtime_error("snapshot: malformed line");
            map_[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
        }
    }

    bool has(const std::string& k) const { return map_.contains(k); }
    std::string str(const std::string& k) const {
        auto it = map_.find(k);
        return it == map_.end() ? std::string() : it->second;
    }
    std::optional<std::string> optStr(const std::string& k) const {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    template <typename T>
    std::optional<T> optInt(const std::string& k) const {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        T v{};
        const auto& s = it->second;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc() || r.ptr != s.data() + s.size()) throw std::runtime_error("snapshot: bad number in " + k);
        return v;
    }
    template <typename T>
    T integer(const std::string& k, T def = 0) const {
        return optInt<T>(k).value_or(def);
    }
    std::optional<bool> optBool(const std::string& k) const {
        auto v = optInt<int>(k);
        if (!v) return std::nullopt;
        return *v != 0;
    }
    bool boolean(const std::string& k) const { return optBool(k).value_or(false); }
    std::optional<double> optDouble(const std::string& k) const {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        double v = 0;
        const auto& s = it->second;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc()) throw std::runtime_error("snapshot: bad number in " + k);
        return v;
    }
    SectionError error(const std::string& prefix) const {
        return SectionError{str(prefix + ".error"), boolean(prefix + ".denied")};
    }

private:
    std::map<std::string, std::string> map_;
};

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (!s.empty() && pos <= s.size()) {
        const size_t end = std::min(s.find(',', pos), s.size());
        out.push_back(s.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

}  // namespace

Snapshot decodeSnapshot(std::string_view text) {
    const Reader r(text);
    if (r.str("v") != "1") throw std::runtime_error("snapshot: unsupported version");

    Snapshot s;
    s.manufacturer = r.str("sys.manufacturer");
    s.product = r.str("sys.product");
    s.sku = r.str("sys.sku");
    s.biosVersion = r.str("sys.bios");
    s.fusion = static_cast<ServiceState>(r.integer<int>("sys.fusion", static_cast<int>(ServiceState::Unknown)));
    s.onAc = r.optBool("sys.ac");
    s.windowsMode = static_cast<PowerMode>(r.integer<int>("sys.mode", static_cast<int>(PowerMode::Unknown)));

    s.thermalError = r.error("th");
    if (!s.thermalError) {
        s.profile = r.integer<uint8_t>("th.profile");
        for (const auto& p : split(r.str("th.profiles"))) {
            uint8_t v = 0;
            if (std::from_chars(p.data(), p.data() + p.size(), v).ec != std::errc())
                throw std::runtime_error("snapshot: bad profile list");
            s.profiles.push_back(v);
        }
        s.gameShiftSupported = r.boolean("th.gs.supported");
        s.gameShift = r.optBool("th.gs");
        for (int i = 0; r.has(std::format("fan.{}.id", i)); ++i) {
            const auto k = std::format("fan.{}.", i);
            SnapshotFan f;
            f.id = r.integer<uint8_t>(k + "id");
            f.rpm = r.optInt<uint32_t>(k + "rpm");
            f.maxRpm = r.optInt<uint32_t>(k + "max");
            f.boost = r.optInt<uint32_t>(k + "boost");
            f.tempC = r.optInt<int>(k + "temp");
            s.fans.push_back(f);
        }
        for (int i = 0; r.has(std::format("sensor.{}.id", i)); ++i) {
            const auto k = std::format("sensor.{}.", i);
            s.extraSensors.push_back({r.integer<uint8_t>(k + "id"), r.optInt<int>(k + "temp")});
        }
    }
    s.awThermal = r.optStr("th.aw");

    s.batteryError = r.error("bat");
    for (int i = 0; r.has(std::format("bat.{}.model", i)); ++i) {
        const auto k = std::format("bat.{}.", i);
        SnapshotBattery b;
        b.model = r.str(k + "model");
        b.manufacturer = r.str(k + "manufacturer");
        b.chemistry = r.str(k + "chemistry");
        b.relativeCapacity = r.boolean(k + "relative");
        b.designCapacity = r.integer<uint32_t>(k + "design");
        b.fullChargeCapacity = r.integer<uint32_t>(k + "full");
        b.remainingCapacity = r.optInt<uint32_t>(k + "remaining");
        b.rate_mW = r.optInt<int32_t>(k + "rate");
        b.cycleCount = r.optInt<uint32_t>(k + "cycles");
        b.temperatureC = r.optDouble(k + "temp");
        b.onLine = r.boolean(k + "online");
        b.charging = r.boolean(k + "charging");
        b.discharging = r.boolean(k + "discharging");
        b.critical = r.boolean(k + "critical");
        s.batteries.push_back(std::move(b));
    }

    s.chargeError = r.error("ch");
    s.chargeExposed = r.boolean("ch.exposed");
    if (s.chargeExposed) {
        s.chargeMode = r.str("ch.mode");
        s.chargeModes = split(r.str("ch.modes"));
        s.chargeStart = r.optInt<int64_t>("ch.start");
        s.chargeStop = r.optInt<int64_t>("ch.stop");
    }
    s.advancedChargeMode = r.optStr("ch.adv");
    s.biosPasswordSet = r.optBool("ch.pw");
    return s;
}

}  // namespace myalien
