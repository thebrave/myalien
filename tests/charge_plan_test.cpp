// Unit tests for planChargeChange (pure logic, no firmware access).
// Checks validation and that stop - start >= 5 holds after every planned write.

#include "core/bios_attributes.h"
#include "core/charge.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace myalien;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

ChargeSettings g15(const wchar_t* mode, int64_t start, int64_t stop) {
    ChargeSettings s;
    s.mode = mode;
    s.modes = {L"Adaptive", L"Standard", L"Express", L"PrimAcUse", L"Custom"};
    s.start = start;
    s.startMin = 50;
    s.startMax = 95;
    s.stop = stop;
    s.stopMin = 55;
    s.stopMax = 100;
    return s;
}

// Simulates the writes and checks the window invariant after each one.
bool windowHolds(ChargeSettings s, const std::vector<AttributeWrite>& writes) {
    for (const auto& w : writes) {
        const std::wstring_view n(w.name);
        if (n == attr::PrimaryBattChargeCfg) s.mode = w.value;
        else if (n == attr::CustomChargeStart) {
            if (s.mode != kCustomMode) return false;  // read-only unless Custom
            s.start = std::stoll(w.value);
        } else if (n == attr::CustomChargeStop) {
            if (s.mode != kCustomMode) return false;
            s.stop = std::stoll(w.value);
        }
        if (s.stop - s.start < kMinChargeWindow) return false;
    }
    return true;
}

template <typename F>
bool throwsInvalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    // Mode only.
    {
        auto w = planChargeChange(g15(L"Adaptive", 50, 80), L"Standard", std::nullopt);
        expect(w.size() == 1 && w[0].value == L"Standard", "mode-only change is one write");
        expect(planChargeChange(g15(L"Standard", 50, 80), L"Standard", std::nullopt).empty(), "no-op is empty");
    }
    // Custom from another mode: mode first.
    {
        const auto cur = g15(L"Adaptive", 50, 80);
        auto w = planChargeChange(cur, L"Custom", ChargeRange{60, 85});
        expect(!w.empty() && std::wstring_view(w[0].name) == attr::PrimaryBattChargeCfg, "Custom mode written first");
        expect(windowHolds(cur, w), "window holds: Adaptive 50-80 -> Custom 60-85");
    }
    // Every reachable transition keeps the invariant.
    {
        int checked = 0;
        for (int s0 = 50; s0 <= 95; s0 += 5)
            for (int e0 = s0 + 5; e0 <= 100; e0 += 5)
                for (int s1 = 50; s1 <= 95; s1 += 5)
                    for (int e1 = s1 + 5; e1 <= 100; e1 += 5) {
                        const auto cur = g15(L"Custom", s0, e0);
                        const auto w = planChargeChange(cur, L"Custom", ChargeRange{s1, e1});
                        if (!windowHolds(cur, w)) {
                            std::printf("FAIL: window broken %d-%d -> %d-%d\n", s0, e0, s1, e1);
                            ++failures;
                        }
                        ++checked;
                    }
        std::printf("checked %d threshold transitions\n", checked);
    }
    // Validation.
    {
        const auto cur = g15(L"Custom", 50, 80);
        expect(throwsInvalid([&] { planChargeChange(cur, L"Turbo", std::nullopt); }), "unknown mode rejected");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Standard", ChargeRange{50, 80}); }),
               "range with non-Custom mode rejected");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{49, 80}); }), "start below min");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{96, 100}); }), "start above max");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{50, 54}); }), "stop below min");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{50, 101}); }), "stop above max");
        expect(throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{70, 74}); }), "window < 5");
        expect(!throwsInvalid([&] { planChargeChange(cur, L"Custom", ChargeRange{70, 75}); }), "window == 5 ok");
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
