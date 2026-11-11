#pragma once

// Token bucket limiting how often the service writes BIOS attributes.
//
// Any interactive user may ask for a charge-setting change, and every attribute write lands in
// the firmware's non-volatile storage. Normal use is a handful of writes now and then; a client
// stuck in a loop (or a hostile one) must not be able to write without bound.

#include <algorithm>
#include <cstdint>

namespace myalien::svc {

class WriteBudget {
public:
    static constexpr unsigned kCapacity = 8;          // burst: a few complete mode + range changes
    static constexpr uint64_t kRefillMs = 60 * 1000;  // then one attribute write per minute

    // Milliseconds until a write is allowed again; 0 when one is allowed now.
    uint64_t waitMs(uint64_t nowMs) {
        refill(nowMs);
        return tokens_ > 0 ? 0 : kRefillMs - (nowMs - lastRefillMs_);
    }

    // Records `writes` attribute writes made at nowMs.
    void spend(unsigned writes, uint64_t nowMs) {
        refill(nowMs);
        tokens_ -= std::min(writes, tokens_);
    }

private:
    void refill(uint64_t nowMs) {
        if (!started_) {
            started_ = true;
            lastRefillMs_ = nowMs;
            return;
        }
        const uint64_t earned = (nowMs - lastRefillMs_) / kRefillMs;
        if (earned == 0) return;
        if (earned >= kCapacity - tokens_) {
            tokens_ = kCapacity;
            lastRefillMs_ = nowMs;  // full: no partial credit carried over
        } else {
            tokens_ += static_cast<unsigned>(earned);
            lastRefillMs_ += earned * kRefillMs;
        }
    }

    unsigned tokens_ = kCapacity;
    uint64_t lastRefillMs_ = 0;
    bool started_ = false;
};

}  // namespace myalien::svc
