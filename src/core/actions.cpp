#include "core/actions.h"

#include "core/awcc.h"
#include "core/bios_attributes.h"
#include "core/error.h"
#include "core/system.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

namespace myalien {

namespace {

struct ProfileAlias {
    const char* name;
    uint8_t id;
};

// User-facing profile names. Only profiles the firmware offers (plus Custom) are accepted by Awcc.
constexpr ProfileAlias kAliases[] = {
    {"balanced", profile::Balanced},       {"balanced-performance", profile::BalancedPerformance},
    {"quiet", profile::Quiet},             {"low-power", profile::LowPower},
    {"cool", profile::Cool},               {"performance", profile::Performance},
    {"custom", profile::Custom},
};

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

const char* aliasOf(uint8_t id) {
    for (const auto& a : kAliases)
        if (a.id == id) return a.name;
    return nullptr;
}

// Small whole number, digits only.
std::optional<int64_t> parseNumber(std::string_view s, size_t maxDigits = 3) {
    if (s.empty() || s.size() > maxDigits) return std::nullopt;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

// Writes are only enabled on the machine everything was verified on.
bool refuseOtherMachine(ActionOutput& o) {
    const auto m = machineInfo();
    if (m.isG15_5520()) return false;
    o.fail(exitcode::Error, std::format("Refusing to write: this machine is {} {}, writes are only enabled on a Dell G15 5520.",
                                        toUtf8(m.manufacturer), toUtf8(m.product)));
    return true;
}

void noteMyDell(ActionOutput& o) {
    if (fusionServiceState() == ServiceState::Running) o.out("Note    : MyDell or the FusionService is already running.");
}

std::string gmodeState(Awcc& awcc) {
    const uint8_t p = awcc.currentProfile();
    const auto gs = awcc.gameShift();
    const char* state = p == profile::GMode ? "on" : gameShiftStuck(p, gs) ? "STUCK (fans at full boost)" : "off";
    return std::format("G-Mode {}, profile {}, Game Shift {}", state, describeProfile(p), gs ? (*gs ? "on" : "off") : "n/a");
}

}  // namespace

// ---------------------------------------------------------------------------------------------
std::string encodeOutput(const ActionOutput& o) {
    std::string r = std::format("exit={}\n", o.exitCode);
    for (const auto& l : o.lines) {
        r += l.error ? "e:" : "o:";
        for (char c : l.text) r += (c == '\r' || c == '\n') ? ' ' : c;
        r += '\n';
    }
    return r;
}

ActionOutput decodeOutput(std::string_view body) {
    ActionOutput o;
    bool haveExit = false;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string_view::npos) end = body.size();
        std::string_view line = body.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;
        if (line.starts_with("exit=")) {
            const auto v = parseNumber(line.substr(5), 2);
            if (!v) throw std::runtime_error("action output: bad exit code");
            o.exitCode = static_cast<int>(*v);
            haveExit = true;
        } else if (line.starts_with("o:")) {
            o.out(std::string(line.substr(2)));
        } else if (line.starts_with("e:")) {
            o.err(std::string(line.substr(2)));
        } else {
            throw std::runtime_error("action output: malformed line");
        }
    }
    if (!haveExit) throw std::runtime_error("action output: missing exit code");
    return o;
}

// ---------------------------------------------------------------------------------------------
std::optional<uint8_t> parseProfileName(std::string_view name) {
    const std::string s = lower(name);
    for (const auto& a : kAliases)
        if (s == a.name) return a.id;
    if (s.size() == 4 && s.starts_with("0x") && std::isxdigit(static_cast<unsigned char>(s[2])) &&
        std::isxdigit(static_cast<unsigned char>(s[3])))
        return static_cast<uint8_t>(std::stoul(s.substr(2), nullptr, 16));
    return std::nullopt;
}

std::string describeProfile(uint8_t id) {
    const char* alias = aliasOf(id);
    return std::format("{} (0x{:02X}){}", profileName(id), id, alias ? std::string("  -> ") + alias : std::string());
}

// ---------------------------------------------------------------------------------------------
ActionOutput actSetProfile(std::string_view profileName) {
    ActionOutput o;
    const auto target = parseProfileName(profileName);
    if (!target) return o.fail(exitcode::Usage, std::format("Unknown profile: {}", profileName));
    if (*target == profile::GMode) return o.fail(exitcode::Usage, "Use `myalien gmode on` for G-Mode.");
    if (refuseOtherMachine(o)) return o;

    Awcc awcc;
    const uint8_t before = awcc.currentProfile();
    if (before == profile::GMode) return o.fail(exitcode::Error, "G-Mode is on. Use `myalien gmode off <profile>` to leave it.");

    const bool stuck = gameShiftStuck(before, awcc.gameShift());
    o.out(std::format("Before  : {}{}", describeProfile(before), stuck ? ", Game Shift stuck on (fans at full boost)" : ""));
    if (before == *target && !stuck) {
        o.out("Already active, nothing to do.");
        return o;
    }
    try {
        // A stuck Game Shift is cleared by leaving G-Mode, not by selecting a profile (see actSetGMode).
        if (stuck) {
            o.out("Note    : entering G-Mode first so leaving it clears Game Shift.");
            awcc.setGMode(true);
        }
        awcc.setProfile(*target);
    } catch (const std::invalid_argument& e) {
        return o.fail(exitcode::Usage, e.what());
    }

    // The firmware, not our write, is the source of truth.
    const uint8_t after = awcc.currentProfile();
    const bool stillStuck = gameShiftStuck(after, awcc.gameShift());
    o.out(std::format("After   : {}{}", describeProfile(after), stillStuck ? ", Game Shift still on" : ""));
    noteMyDell(o);
    if (after != *target)
        return o.fail(exitcode::NotApplied, std::format("Profile did not change as requested (firmware reports 0x{:02X}).", after));
    if (stillStuck)
        return o.fail(exitcode::NotApplied, "Game Shift is still on: the fans stay at full boost. Try `myalien gmode off <profile>`.");
    o.out("OK");
    return o;
}

ActionOutput actSetGMode(bool on, std::string_view profileWhenOff) {
    ActionOutput o;
    const auto offProfile = parseProfileName(profileWhenOff);
    if (!on && !offProfile) return o.fail(exitcode::Usage, std::format("Unknown profile: {}", profileWhenOff));
    if (refuseOtherMachine(o)) return o;

    Awcc awcc;
    if (!awcc.caps().gameShift) return o.fail(exitcode::Error, "G-Mode is not supported on this machine.");

    o.out(std::format("Before  : {}", gmodeState(awcc)));
    const uint8_t wanted = on ? profile::GMode : *offProfile;
    const uint8_t before = awcc.currentProfile();
    const bool stuck = gameShiftStuck(before, awcc.gameShift());
    if (before == wanted && !stuck) {
        o.out("Already in that state, nothing to do.");
        return o;
    }
    try {
        // Leaving a stuck Game Shift: selecting a profile alone does not clear it, but leaving
        // G-Mode does (verified on the G15 5520), so enter G-Mode first.
        if (!on && stuck) {
            o.out("Note    : Game Shift is on outside G-Mode; entering G-Mode first so leaving it clears Game Shift.");
            awcc.setGMode(true);
        }
        awcc.setGMode(on, on ? profile::Balanced : *offProfile);
    } catch (const std::invalid_argument& e) {
        return o.fail(exitcode::Usage, e.what());
    }

    const uint8_t after = awcc.currentProfile();
    const bool stillStuck = gameShiftStuck(after, awcc.gameShift());
    o.out(std::format("After   : {}", gmodeState(awcc)));
    noteMyDell(o);
    if (after != wanted)
        return o.fail(exitcode::NotApplied,
                      on ? "Firmware did not report G-Mode after the request."
                         : "Firmware still reports another profile. If the machine is hot, the firmware may keep G-Mode "
                           "for about a minute; check `myalien gmode` again shortly.");
    if (stillStuck)
        return o.fail(exitcode::NotApplied,
                      "Game Shift is still on: the fans stay at full boost. Try again shortly, or toggle G-Mode in MyDell.");
    o.out("OK");
    return o;
}

ActionOutput actSetFanBoost(std::string_view fans, std::string_view boostText) {
    ActionOutput o;
    const auto boost = parseNumber(boostText);
    if (!boost || *boost > kMaxFanBoost)
        return o.fail(exitcode::Usage, std::format("Boost must be a whole number from 0 to {}.", kMaxFanBoost));
    if (refuseOtherMachine(o)) return o;

    Awcc awcc;
    const std::string which = lower(fans);
    std::vector<uint8_t> ids;
    for (const auto& f : awcc.caps().fans)
        if (which == "all" || which == lower(fanName(f.id))) ids.push_back(f.id);
    if (ids.empty()) return o.fail(exitcode::Usage, std::format("Unknown fan: {} (cpu, gpu or all)", fans));

    const uint8_t p = awcc.currentProfile();
    if (p != profile::Custom)
        return o.fail(exitcode::Error,
                      std::format("Fan boost is only changed in the Custom profile (current: {}). Run `myalien profile set custom` first.",
                                  describeProfile(p)));

    bool ok = true;
    for (uint8_t id : ids) {
        const auto before = awcc.fan(id);
        awcc.setFanBoost(id, static_cast<uint8_t>(*boost));
        const auto after = awcc.fan(id);
        o.out(std::format("Set     : {} boost {} -> {}  (reads {})", fanName(id),
                          before.boost ? std::to_string(*before.boost) : "n/a", *boost,
                          after.boost ? std::to_string(*after.boost) : "n/a"));
        ok &= after.boost && *after.boost == static_cast<uint32_t>(*boost);
    }
    noteMyDell(o);
    if (!ok) return o.fail(exitcode::NotApplied, "A fan does not report the requested boost.");
    o.out("OK");
    return o;
}

// ---------------------------------------------------------------------------------------------
ChargeActionResult actSetChargeMode(std::string_view modeText, std::optional<std::pair<std::string, std::string>> rangeText) {
    ChargeActionResult result;
    ActionOutput& o = result.output;

    std::optional<ChargeRange> range;
    if (rangeText) {
        const auto start = parseNumber(rangeText->first);
        const auto stop = parseNumber(rangeText->second);
        if (!start || !stop) {
            o.fail(exitcode::Usage, "Start and stop must be whole percentages, e.g. `battery mode custom 50 80`.");
            return result;
        }
        range = ChargeRange{*start, *stop};
    }
    if (refuseOtherMachine(o)) return result;

    BiosAttributes bios;
    const ChargeSettings cur = readChargeSettings(bios);

    // CLI name -> BIOS spelling offered by this machine.
    std::string want = lower(modeText);
    if (want == "ac" || want == "primarily-ac") want = "primacuse";
    std::optional<std::wstring> mode;
    for (const auto& m : cur.modes)
        if (lower(toUtf8(m)) == want) mode = m;
    if (!mode) {
        o.fail(exitcode::Usage, std::format("Unknown charge mode: {} (adaptive, standard, express, primacuse, custom)", modeText));
        return result;
    }

    std::vector<AttributeWrite> plan;
    try {
        plan = planChargeChange(cur, *mode, range);
    } catch (const std::invalid_argument& e) {
        o.fail(exitcode::Usage, e.what());
        return result;
    }

    auto describe = [](const ChargeSettings& s) {
        return std::format("{} (custom range {}-{} %{})", toUtf8(s.mode), s.start, s.stop,
                           s.mode == kCustomMode ? "" : ", inactive");
    };
    o.out(std::format("Before  : {}", describe(cur)));
    if (plan.empty()) {
        o.out("Already set, nothing to do.");
        result.after = cur;
        return result;
    }

    // One attribute at a time, each read back before the next: the thresholds only become
    // writable once the BIOS reports the Custom mode.
    for (const auto& w : plan) {
        ++result.writes;
        bios.set(w.name, w.value);
        std::wstring got;
        if (std::wstring_view(w.name) == attr::PrimaryBattChargeCfg) {
            const auto a = bios.enumeration(w.name);
            got = a ? a->current : L"?";
        } else {
            const auto a = bios.integer(w.name);
            got = a ? std::to_wstring(a->current) : L"?";
        }
        o.out(std::format("Set     : {} = {}  -> reads {}", toUtf8(w.name), toUtf8(w.value), toUtf8(got)));
        if (got != w.value) {
            o.fail(exitcode::NotApplied,
                   "The BIOS accepted the write but does not report the new value yet; it may apply after a reboot. Stopping here.");
            return result;
        }
    }

    const ChargeSettings after = readChargeSettings(bios);
    o.out(std::format("After   : {}", describe(after)));
    noteMyDell(o);
    o.out("OK");
    result.after = after;
    return result;
}

}  // namespace myalien
