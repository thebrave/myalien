// Round-trip and robustness tests for the snapshot wire format.

#include "core/snapshot.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace myalien;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

Snapshot sample() {
    Snapshot s;
    s.manufacturer = "Dell Inc.";
    s.product = "Dell G15 5520";
    s.sku = "0B56";
    s.biosVersion = "1.44.0";
    s.fusion = ServiceState::Running;
    s.onAc = true;
    s.windowsMode = PowerMode::Balanced;
    s.profile = 0xA0;
    s.profiles = {0xA0, 0xA1, 0xA5, 0xA3};
    s.gameShiftSupported = true;
    s.gameShift = false;
    s.fans = {{0x32, 655u, 4800u, 0u, 54}, {0x33, std::nullopt, 4800u, 30u, std::nullopt}};
    s.extraSensors = {{0x03, 41}};
    s.awThermal = "Balanced";
    SnapshotBattery b;
    b.model = "DELL 70N2F32";
    b.manufacturer = "SMP";
    b.chemistry = "LiP";
    b.designCapacity = 84280;
    b.fullChargeCapacity = 66770;
    b.remainingCapacity = 66770;
    b.rate_mW = -12500;
    b.temperatureC = 31.25;
    b.onLine = true;
    s.batteries = {b};
    s.chargeExposed = true;
    s.chargeMode = "Adaptive";
    s.chargeModes = {"Adaptive", "Standard", "Express", "PrimAcUse", "Custom"};
    s.chargeStart = 50;
    s.chargeStop = 80;
    s.advancedChargeMode = "Disabled";
    s.biosPasswordSet = false;
    return s;
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    {
        const Snapshot a = sample();
        const std::string wire = encodeSnapshot(a);
        const Snapshot b = decodeSnapshot(wire);
        expect(encodeSnapshot(b) == wire, "encode(decode(encode(x))) is stable");
        expect(b.product == "Dell G15 5520" && b.isG15_5520(), "system fields");
        expect(b.fusion == ServiceState::Running && b.onAc == true && b.windowsMode == PowerMode::Balanced, "enums");
        expect(b.profile == 0xA0 && b.profiles.size() == 4 && b.profiles[3] == 0xA3, "profiles");
        expect(b.gameShift == false && b.gameShiftSupported, "game shift");
        expect(b.fans.size() == 2 && b.fans[0].rpm == 655u && b.fans[0].tempC == 54, "fan 0");
        expect(!b.fans[1].rpm && b.fans[1].boost == 30u && !b.fans[1].tempC, "fan 1 optional fields");
        expect(b.extraSensors.size() == 1 && b.extraSensors[0].tempC == 41, "extra sensor");
        expect(b.batteries.size() == 1 && b.batteries[0].rate_mW == -12500, "battery rate");
        expect(b.batteries[0].temperatureC && *b.batteries[0].temperatureC > 31.2 && *b.batteries[0].temperatureC < 31.3,
               "battery temperature");
        expect(b.chargeModes.size() == 5 && b.chargeStart == 50 && b.chargeStop == 80, "charge");
        expect(b.biosPasswordSet == false && b.advancedChargeMode == "Disabled", "charge extras");
    }
    {
        Snapshot a = sample();
        a.thermalError = {"WMI: access denied (run elevated)", true};
        a.chargeError = {"line1\nline2", false};
        const Snapshot b = decodeSnapshot(encodeSnapshot(a));
        expect(b.thermalError && b.thermalError.accessDenied, "thermal error round-trips");
        expect(b.fans.empty(), "no thermal data with a thermal error");
        expect(b.chargeError.message == "line1 line2", "newlines in values are flattened");
    }
    {
        expect(throws([] { decodeSnapshot("v=2\n"); }), "unknown version rejected");
        expect(throws([] { decodeSnapshot("garbage\n"); }), "line without '=' rejected");
        expect(throws([] { decodeSnapshot("v=1\nth.profile=abc\n"); }), "bad number rejected");
        expect(!throws([] { decodeSnapshot("v=1\nfuture.key=x\n"); }), "unknown keys ignored");
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
