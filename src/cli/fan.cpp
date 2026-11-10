// `myalien fan [boost <fan> <0-100>]` - fan readings and manual boost (Custom profile only).

#include "cli/commands.h"
#include "cli/common.h"
#include "core/awcc.h"
#include "core/error.h"
#include "core/wmi.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace myalien::cli {

namespace {

int usage() {
    std::fputs(
        "Usage:\n"
        "  myalien fan                          Show fan speed, boost and linked temperature\n"
        "  myalien fan boost <fan> <0-100>      Set a fan's boost (elevated, Custom profile only)\n"
        "\n"
        "<fan>: cpu | gpu | all\n"
        "Switch to Custom first: `myalien profile set custom`. Boost 0 means no extra boost.\n",
        stderr);
    return 2;
}

std::wstring lower(std::wstring_view s) {
    std::wstring r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
}

std::optional<std::vector<uint8_t>> parseFans(std::wstring_view arg, const AwccCapabilities& caps) {
    const std::wstring a = lower(arg);
    std::vector<uint8_t> out;
    for (const auto& f : caps.fans)
        if (a == L"all" || a == lower(toWide(fanName(f.id)))) out.push_back(f.id);
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<uint8_t> parseBoost(std::wstring_view arg) {
    if (arg.empty() || arg.size() > 3) return std::nullopt;
    unsigned v = 0;
    for (wchar_t c : arg) {
        if (c < L'0' || c > L'9') return std::nullopt;
        v = v * 10 + static_cast<unsigned>(c - L'0');
    }
    if (v > kMaxFanBoost) return std::nullopt;
    return static_cast<uint8_t>(v);
}

std::string fanLine(Awcc& awcc, const AwccFan& f) {
    const auto r = awcc.fan(f.id);
    std::string temp = "n/a";
    if (!f.sensors.empty())
        if (const auto t = awcc.temperatureC(f.sensors.front())) temp = std::format("{} °C", *t);
    auto n = [](const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("n/a"); };
    return std::format("  {:<4}: {:>4} rpm (max {}), boost {:>3}, {}", fanName(f.id), n(r.rpm), n(r.maxRpm),
                       n(r.boost), temp);
}

void printFans(Awcc& awcc) {
    const uint8_t p = awcc.currentProfile();
    std::printf("Profile : %s\n", describeProfile(p).c_str());
    for (const auto& f : awcc.caps().fans) std::printf("%s\n", fanLine(awcc, f).c_str());
}

int setBoost(std::wstring_view fanArg, std::wstring_view boostArg) {
    const auto boost = parseBoost(boostArg);
    if (!boost) {
        std::fprintf(stderr, "Boost must be a whole number from 0 to %u.\n\n", kMaxFanBoost);
        return usage();
    }
    if (const auto refused = refuseWrite("Changing fan boost")) return *refused;

    Awcc awcc;
    const auto fans = parseFans(fanArg, awcc.caps());
    if (!fans) {
        std::fprintf(stderr, "Unknown fan: %s\n\n", toUtf8(fanArg).c_str());
        return usage();
    }
    const uint8_t p = awcc.currentProfile();
    if (p != profile::Custom) {
        std::fprintf(stderr, "Fan boost is only changed in the Custom profile (current: %s).\n"
                             "Run `myalien profile set custom` first.\n",
                     describeProfile(p).c_str());
        return 1;
    }

    std::puts("Before  :");
    for (const auto& f : awcc.caps().fans) std::printf("%s\n", fanLine(awcc, f).c_str());

    bool ok = true;
    for (uint8_t id : *fans) {
        awcc.setFanBoost(id, *boost);
        const auto r = awcc.fan(id);
        std::printf("Set     : %s boost = %u  -> reads %s\n", fanName(id), *boost,
                    r.boost ? std::to_string(*r.boost).c_str() : "n/a");
        ok &= r.boost && *r.boost == *boost;
    }

    // On the G15 5520 fans start reacting after ~3-4 s and settle a few seconds later;
    // sample the speed so the effect is visible.
    for (int s = 1; s <= 8; ++s) {
        Sleep(1000);
        std::string line = std::format("+{}s     :", s);
        for (uint8_t id : *fans) {
            const auto r = awcc.fan(id);
            line += std::format("  {} {} rpm", fanName(id), r.rpm ? std::to_string(*r.rpm) : std::string("n/a"));
        }
        std::printf("%s\n", line.c_str());
    }
    noteMyDell();

    if (!ok) {
        std::fputs("A fan does not report the requested boost.\n", stderr);
        return 3;
    }
    std::puts("OK");
    return 0;
}

}  // namespace

int cmdFan(std::span<const std::wstring_view> args) {
    wmi::ComApartment com;

    if (args.empty()) {
        Awcc awcc;
        printFans(awcc);
        return 0;
    }
    if (args[0] == L"boost" && args.size() == 3) return setBoost(args[1], args[2]);
    return usage();
}

}  // namespace myalien::cli
