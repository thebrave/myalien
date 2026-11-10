// `myalien battery [mode <mode> [<start> <stop>]]` - battery health and BIOS charge mode.

#include "cli/commands.h"
#include "cli/common.h"
#include "core/bios_attributes.h"
#include "core/charge.h"
#include "core/error.h"
#include "core/wmi.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <format>
#include <optional>
#include <string>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien battery                              Show battery health and charge mode\n"
        "  myalien battery mode <mode>                  Set the charge mode (elevated prompt required)\n"
        "  myalien battery mode custom <start> <stop>   Charge only between start % and stop %\n"
        "\n"
        "<mode>: adaptive | standard | express | primacuse (mostly on AC) | custom\n"
        "Custom range: start 50-95, stop 55-100, stop at least 5 above start.\n"
        "The setting is stored in the BIOS; MyDell shows the same setting.\n",
        stderr);
    return 2;
}

std::wstring lower(std::wstring_view s) {
    std::wstring r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
}

// Maps a CLI name onto the BIOS spelling offered by this machine.
std::optional<std::wstring> resolveMode(std::wstring_view arg, const ChargeSettings& s) {
    std::wstring want = lower(arg);
    if (want == L"ac" || want == L"primarily-ac") want = L"primacuse";
    for (const auto& m : s.modes)
        if (lower(m) == want) return m;
    return std::nullopt;
}

std::optional<int64_t> parsePercent(std::wstring_view arg) {
    if (arg.empty() || arg.size() > 3) return std::nullopt;
    int64_t v = 0;
    for (wchar_t c : arg) {
        if (c < L'0' || c > L'9') return std::nullopt;
        v = v * 10 + (c - L'0');
    }
    return v;
}

std::string describe(const ChargeSettings& s) {
    return std::format("{} (custom range {}-{} %{})", toUtf8(s.mode), s.start, s.stop,
                       s.mode == kCustomMode ? "" : ", inactive");
}

std::wstring readBack(const BiosAttributes& bios, const wchar_t* name) {
    if (std::wstring_view(name) == attr::PrimaryBattChargeCfg) {
        const auto a = bios.enumeration(name);
        return a ? a->current : L"?";
    }
    const auto a = bios.integer(name);
    return a ? std::to_wstring(a->current) : L"?";
}

int setMode(std::span<const std::wstring_view> args) {
    if (args.size() != 1 && args.size() != 3) return usage();
    if (const auto refused = refuseWrite("Changing the charge mode")) return *refused;

    BiosAttributes bios;
    const ChargeSettings cur = readChargeSettings(bios);

    const auto mode = resolveMode(args[0], cur);
    if (!mode) {
        std::fprintf(stderr, "Unknown charge mode: %s\n\n", toUtf8(args[0]).c_str());
        return usage();
    }
    std::optional<ChargeRange> range;
    if (args.size() == 3) {
        const auto start = parsePercent(args[1]);
        const auto stop = parsePercent(args[2]);
        if (!start || !stop) {
            std::fputs("Start and stop must be whole percentages, e.g. `battery mode custom 50 80`.\n\n", stderr);
            return usage();
        }
        range = ChargeRange{*start, *stop};
    }

    std::vector<AttributeWrite> plan;
    try {
        plan = planChargeChange(cur, *mode, range);
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }

    std::printf("Before  : %s\n", describe(cur).c_str());
    if (plan.empty()) {
        std::puts("Already set, nothing to do.");
        return 0;
    }

    // One attribute at a time, each read back before the next: the thresholds only become
    // writable once the BIOS reports the Custom mode.
    for (const auto& w : plan) {
        bios.set(w.name, w.value);
        const std::wstring got = readBack(bios, w.name);
        std::printf("Set     : %s = %s  -> reads %s\n", toUtf8(w.name).c_str(), toUtf8(w.value).c_str(),
                    toUtf8(got).c_str());
        if (got != w.value) {
            std::fputs("The BIOS accepted the write but does not report the new value yet; it may apply after a "
                       "reboot. Stopping here.\n",
                       stderr);
            return 3;
        }
    }

    const ChargeSettings after = readChargeSettings(bios);
    std::printf("After   : %s\n", describe(after).c_str());
    noteMyDell();
    std::puts("OK");
    return 0;
}

}  // namespace

int cmdBattery(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;

    if (args.empty()) {
        std::optional<BiosAttributes> bios;
        std::string err;
        try {
            bios.emplace();
            (void)bios->enumeration(attr::PrimaryBattChargeCfg);  // surfaces access denied early
        } catch (const HrError& e) {
            err = e.accessDenied() ? "unavailable (access denied - run from an elevated prompt)" : e.what();
            bios.reset();
        }
        printBatterySection(bios ? &*bios : nullptr, err);
        return 0;
    }
    if (args[0] == L"mode" && args.size() >= 2) return setMode(args.subspan(1));
    return usage();
}

}  // namespace myalien::cli
