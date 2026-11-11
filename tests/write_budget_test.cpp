// Tests for the service's BIOS write budget (token bucket).

#include "svc/write_budget.h"

#include <cstdio>
#include <cstdlib>

using namespace myalien::svc;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint64_t kMin = WriteBudget::kRefillMs;

}  // namespace

int main() {
    {
        WriteBudget b;
        uint64_t t = 1000;
        for (unsigned i = 0; i < WriteBudget::kCapacity; ++i) {
            expect(b.waitMs(t) == 0, "burst allowed");
            b.spend(1, t);
        }
        expect(b.waitMs(t) == kMin, "empty: wait a full refill period");
        expect(b.waitMs(t + kMin - 1) == 1, "wait shrinks with time");
        expect(b.waitMs(t + kMin) == 0, "one write allowed after a period");
        b.spend(1, t + kMin);
        expect(b.waitMs(t + kMin) == kMin, "and only one");
    }
    {
        // A request may write several attributes: overspending empties the bucket, no debt.
        WriteBudget b;
        expect(b.waitMs(0) == 0, "fresh budget");
        b.spend(WriteBudget::kCapacity + 5, 0);
        expect(b.waitMs(0) == kMin, "overspend empties");
        expect(b.waitMs(kMin) == 0, "no debt carried");
    }
    {
        // Idle time refills up to the capacity, not beyond.
        WriteBudget b;
        b.spend(WriteBudget::kCapacity, 0);
        const uint64_t later = 1000 * kMin;
        for (unsigned i = 0; i < WriteBudget::kCapacity; ++i) {
            expect(b.waitMs(later) == 0, "refilled after idle");
            b.spend(1, later);
        }
        expect(b.waitMs(later) > 0, "capped at capacity");
    }
    {
        // Requests that write nothing cost nothing.
        WriteBudget b;
        for (int i = 0; i < 100; ++i) b.spend(0, static_cast<uint64_t>(i));
        expect(b.waitMs(100) == 0, "no-op requests are free");
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
