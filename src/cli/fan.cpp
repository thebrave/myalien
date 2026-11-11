// `myalien fan [boost <fan> <0-100>]` - fan readings and manual boost (Custom profile only).

#include "cli/commands.h"
#include "cli/common.h"
#include "core/actions.h"
#include "core/awcc.h"
#include "core/wmi.h"

#include <windows.h>

#include <cstdio>
#include <format>
#include <string>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien fan                          Show fan speed, boost and linked temperature\n"
        "  myalien fan boost <fan> <0-100>      Set a fan's boost (Custom profile only)\n"
        "\n"
        "<fan>: cpu | gpu | all\n"
        "Switch to Custom first: `myalien profile set custom`. Boost 0 means no extra boost.\n"
        "Changes run through the MyAlien service, or in-process from an elevated prompt.\n",
        stderr);
    return exitcode::Usage;
}

std::string opt(const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("n/a"); }

int show() {
    const auto [s, source] = obtainSnapshot({.battery = false, .charge = false});
    if (s.thermalError) {
        std::fprintf(stderr, "Thermal data unavailable: %s\n", s.thermalError.message.c_str());
        return exitcode::Error;
    }
    std::printf("Source  : %s\n", source.c_str());
    std::printf("Profile : %s\n", describeProfile(s.profile).c_str());
    for (const auto& f : s.fans)
        std::printf("  %-4s: %4s rpm (max %s), boost %3s, %s\n", fanName(f.id), opt(f.rpm).c_str(),
                    opt(f.maxRpm).c_str(), opt(f.boost).c_str(),
                    f.tempC ? std::format("{} °C", *f.tempC).c_str() : "n/a");
    return exitcode::Ok;
}

// Fans react after 3-4 s on the G15 5520: sample their speed so the effect is visible.
void sampleSpeeds() {
    for (int i = 1; i <= 8; ++i) {
        Sleep(1000);
        const auto [s, source] = obtainSnapshot({.battery = false, .charge = false});
        if (s.thermalError) return;
        std::string line = std::format("+{}s     :", i);
        for (const auto& f : s.fans) line += std::format("  {} {} rpm", fanName(f.id), opt(f.rpm));
        std::puts(line.c_str());
    }
}

}  // namespace

int cmdFan(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;
    if (args.empty()) return show();
    if (args[0] == L"boost" && args.size() == 3) {
        const std::string fans = toAscii(args[1]);
        const std::string boost = toAscii(args[2]);
        const int r = runWrite("Changing fan boost", "FAN " + fans + " " + boost,
                               [&] { return actSetFanBoost(fans, boost); });
        if (r == exitcode::Ok) sampleSpeeds();
        return r;
    }
    return usage();
}

}  // namespace myalien::cli
