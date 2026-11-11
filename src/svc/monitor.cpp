#include "svc/monitor.h"

#include "core/error.h"
#include "core/system.h"
#include "core/wmi.h"
#include "svc/context.h"
#include "svc/log.h"

namespace myalien::svc {

namespace {

// Applies `wanted` with the same firmware calls the actions use (validated by Awcc).
void reapply(Awcc& awcc, const ThermalState& wanted, const ThermalState& seen) {
    if (wanted.profile == profile::GMode) {
        awcc.setGMode(true);
        return;
    }
    // A stuck Game Shift is cleared by leaving G-Mode, not by selecting a profile.
    if (gameShiftStuck(seen.profile, seen.gameShift)) awcc.setGMode(true);
    awcc.setProfile(wanted.profile);  // also leaves G-Mode
    if (wanted.profile == profile::Custom)
        for (const auto& [fan, boost] : wanted.boosts) awcc.setFanBoost(fan, static_cast<uint8_t>(boost));
}

}  // namespace

void Monitor::recordWrite(const ThermalState& now) {
    std::lock_guard lock(mutex_);
    desired_.thermal = now;
    desired_.overridden = false;
    lastSeen_ = now;
    if (!statePath_.empty()) saveDesired(statePath_, desired_);
    logf("monitor: MyAlien setting is now {}", describe(now));
}

void Monitor::observe(Awcc& awcc, const char* reason, bool mayReapply) {
    const ThermalState seen = readThermalState(awcc);
    std::lock_guard lock(mutex_);

    const bool myDell = fusionServiceState() == ServiceState::Running;
    const bool changed = !lastSeen_ || !sameSetting(*lastSeen_, seen);

    if (!mayReapply) {
        // Periodic poll: anything new here was not made through MyAlien (recordWrite updates
        // lastSeen_ for our own writes).
        if (changed && lastSeen_) {
            logf("monitor: external change: {} -> {}{}", describe(*lastSeen_), describe(seen),
                 myDell ? " (MyDell is running)" : "");
            if (desired_.thermal && !desired_.overridden && !sameSetting(*desired_.thermal, seen)) {
                desired_.overridden = true;
                if (!statePath_.empty()) saveDesired(statePath_, desired_);
                logLine("monitor: MyAlien setting overridden; not re-applied until the next change made through MyAlien");
            }
        }
        lastSeen_ = seen;
        return;
    }

    // Start-up / resume: report what the firmware kept, and re-apply if appropriate.
    const std::string wanted = desired_.thermal ? describe(*desired_.thermal) : std::string("none");
    logf("monitor: {}: firmware reports {}; MyAlien setting {}{}", reason, describe(seen), wanted,
         desired_.overridden ? " (overridden)" : "");
    lastSeen_ = seen;

    if (!desired_.thermal || desired_.overridden || sameSetting(*desired_.thermal, seen)) return;
    if (myDell) {
        logLine("monitor: MyDell is running and manages persistence: not re-applying");
        return;
    }
    if (!machineInfo().isG15_5520()) return;

    try {
        reapply(awcc, *desired_.thermal, seen);
        const ThermalState after = readThermalState(awcc);
        lastSeen_ = after;
        logf("monitor: re-applied {} -> firmware reports {}{}", wanted, describe(after),
             sameSetting(*desired_.thermal, after) ? "" : " (NOT applied)");
    } catch (const std::exception& e) {
        logf("monitor: re-apply failed: {}", e.what());
    }
}

void Monitor::run(ServiceContext& ctx) {
    wmi::ComApartment com;
    {
        std::lock_guard lock(mutex_);
        statePath_ = defaultStatePath();
        desired_ = loadDesired(statePath_);
    }

    std::optional<Awcc> awcc;
    auto step = [&](const char* reason, bool mayReapply) {
        try {
            if (!awcc) awcc.emplace();
            observe(*awcc, reason, mayReapply);
        } catch (const std::exception& e) {
            awcc.reset();  // reconnect next time
            logf("monitor: {} failed: {}", reason, e.what());
        }
    };

    step("start-up", true);
    for (;;) {
        const HANDLE handles[] = {ctx.stopEvent, ctx.resumeEvent};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, kPollMs);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        if (w == WAIT_OBJECT_0 + 1) {
            if (WaitForSingleObject(ctx.stopEvent, kResumeSettleMs) == WAIT_OBJECT_0) break;
            step("resume", true);
        } else {
            step("poll", false);
        }
    }
}

}  // namespace myalien::svc
