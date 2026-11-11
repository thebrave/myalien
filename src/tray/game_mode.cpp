#include "tray/game_mode.h"

namespace myalien::tray {

namespace {

bool restorable(PowerMode m) {
    return m == PowerMode::BestEfficiency || m == PowerMode::Balanced || m == PowerMode::BestPerformance;
}

}  // namespace

GameStep GameModeController::update(bool gameInFront, const GameCurrent& now, const GameTargets& targets,
                                    uint64_t nowMs) {
    GameStep step;
    if (gameInFront) {
        leaveAt_.reset();
        if (pending_) return step;  // already engaged (same or another game)

        GamePending p;
        // Only what differs is changed, so only that is restored later. An unknown current value
        // cannot be restored, so that part is left alone.
        if (targets.power && restorable(now.power) && now.power != *targets.power) {
            p.savedPower = now.power;
            p.appliedPower = *targets.power;
            step.setPower = *targets.power;
        }
        if (targets.profile && now.profile && *now.profile != *targets.profile) {
            p.savedProfile = *now.profile;
            p.appliedProfile = *targets.profile;
            step.setProfile = *targets.profile;
        }
        pending_ = p;
        step.kind = GameStep::Kind::Enter;
        step.savePending = p.changedSomething();
        return step;
    }

    if (!pending_) return step;
    if (!leaveAt_) {
        leaveAt_ = nowMs + kLeaveDelayMs;
        return step;
    }
    if (nowMs < *leaveAt_) return step;
    return restoreNow(now);
}

GameStep GameModeController::restoreNow(const GameCurrent& now) {
    GameStep step;
    if (!pending_) return step;
    const GamePending p = *pending_;
    pending_.reset();
    leaveAt_.reset();
    step.kind = GameStep::Kind::Restore;
    step.clearPending = true;
    // Restore a part only if it still has the value game mode set: a change made meanwhile (tray
    // menu, Fn+F9, MyDell, Settings) is the user's choice and wins.
    if (p.appliedPower && p.savedPower && now.power == *p.appliedPower) step.setPower = *p.savedPower;
    if (p.appliedProfile && p.savedProfile && now.profile == *p.appliedProfile) step.setProfile = *p.savedProfile;
    return step;
}

}  // namespace myalien::tray
