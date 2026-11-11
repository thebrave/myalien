// Tests for the service's remembered thermal setting: comparison rule and state.ini round-trip.

#include "svc/thermal_state.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace myalien;
using namespace myalien::svc;

namespace {

int failures = 0;

constexpr uint8_t F(int id) { return static_cast<uint8_t>(id); }

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::wstring tempIni() {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"myalien_state_test.ini";
}

}  // namespace

int main() {
    {
        const ThermalState quiet{profile::Quiet, {{F(0x32), 0u}, {F(0x33), 0u}}};
        const ThermalState quietOtherBoost{profile::Quiet, {{F(0x32), 40u}, {F(0x33), 0u}}};
        const ThermalState custom30{profile::Custom, {{F(0x32), 30u}, {F(0x33), 30u}}};
        const ThermalState custom40{profile::Custom, {{F(0x32), 40u}, {F(0x33), 30u}}};
        const ThermalState gmode{profile::GMode, {{F(0x32), 100u}, {F(0x33), 100u}}};
        expect(sameSetting(quiet, quietOtherBoost), "boosts ignored outside Custom");
        expect(!sameSetting(custom30, custom40), "boosts compared in Custom");
        expect(sameSetting(custom30, custom30), "identical Custom");
        expect(!sameSetting(quiet, gmode), "different profiles");
        expect(describe(custom30) == "Custom (0x00), boost CPU 30 GPU 30", "describe Custom");
        expect(describe(quiet) == "Quiet (0xA3)", "describe Quiet");

        // Game Shift: a remembered setting (gameShift unset) implies on for G-Mode, off otherwise.
        const ThermalState quietStuck{profile::Quiet, {}, true};
        const ThermalState quietClean{profile::Quiet, {}, false};
        const ThermalState gmodeRead{profile::GMode, {}, true};
        const ThermalState gmodeRemembered{profile::GMode, {}, std::nullopt};
        expect(!sameSetting(quiet, quietStuck), "stuck Game Shift differs from the remembered Quiet");
        expect(sameSetting(quiet, quietClean), "Game Shift off matches the remembered Quiet");
        expect(sameSetting(gmodeRemembered, gmodeRead), "G-Mode implies Game Shift on");
        expect(describe(quietStuck) == "Quiet (0xA3), Game Shift stuck on", "describe stuck");
    }
    {
        const std::wstring path = tempIni();
        DeleteFileW(path.c_str());
        expect(!loadDesired(path).thermal, "missing file = nothing chosen");

        DesiredState d;
        d.thermal = ThermalState{profile::Custom, {{F(0x32), 30u}, {F(0x33), 45u}}};
        d.overridden = true;
        saveDesired(path, d);
        const DesiredState r = loadDesired(path);
        expect(r.thermal && r.thermal->profile == profile::Custom, "profile round-trips");
        expect(r.thermal && r.thermal->boosts.size() == 2 && r.thermal->boosts.at(0x33) == 45, "boosts round-trip");
        expect(r.overridden, "overridden round-trips");

        d.thermal = ThermalState{profile::Quiet, {}};
        d.overridden = false;
        saveDesired(path, d);
        const DesiredState q = loadDesired(path);
        expect(q.thermal && q.thermal->profile == profile::Quiet && q.thermal->boosts.empty() && !q.overridden,
               "overwrite with fewer fields");

        WritePrivateProfileStringW(L"thermal", L"boosts", L"50:30,junk,51:999,52:x", path.c_str());
        const DesiredState j = loadDesired(path);
        expect(j.thermal && j.thermal->boosts.size() == 2 && j.thermal->boosts.at(50) == 30 && !j.thermal->boosts.contains(51),
               "malformed / out-of-range boosts ignored");
        DeleteFileW(path.c_str());
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
