// Tests for the game mode decisions (src/tray/game_mode.cpp): enter, leave delay, restore only
// what is unchanged, resume after a crash.

#include "tray/game_mode.h"

#include <cstdio>

using namespace myalien;
using namespace myalien::tray;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint8_t kQuiet = 0xA3, kBalanced = 0xA0, kGMode = 0xAB;
constexpr uint64_t kDelay = GameModeController::kLeaveDelayMs;

GameCurrent cur(PowerMode p, std::optional<uint8_t> profile) { return {p, profile}; }

}  // namespace

int main() {
    const GameTargets targets;  // Best performance + G-Mode

    // Enter, leave after the delay, restore both.
    {
        GameModeController c;
        auto s = c.update(true, cur(PowerMode::Balanced, kQuiet), targets, 0);
        expect(s.kind == GameStep::Kind::Enter && s.setPower == PowerMode::BestPerformance && s.setProfile == kGMode,
               "enter sets both targets");
        expect(s.savePending && c.active(), "enter saves the record");
        expect(c.update(true, cur(PowerMode::BestPerformance, kGMode), targets, 100).kind == GameStep::Kind::None,
               "still in the game: nothing");

        s = c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 1000);
        expect(s.kind == GameStep::Kind::None && c.leaving(), "leaving starts the delay");
        s = c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 1000 + kDelay - 1);
        expect(s.kind == GameStep::Kind::None, "nothing before the delay ends");
        s = c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 1000 + kDelay);
        expect(s.kind == GameStep::Kind::Restore && s.setPower == PowerMode::Balanced && s.setProfile == kQuiet,
               "restore both after the delay");
        expect(s.clearPending && !c.active(), "restore clears the record");
    }

    // Alt-tab back within the delay cancels the restore.
    {
        GameModeController c;
        c.update(true, cur(PowerMode::Balanced, kQuiet), targets, 0);
        c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 1000);
        auto s = c.update(true, cur(PowerMode::BestPerformance, kGMode), targets, 2000);
        expect(s.kind == GameStep::Kind::None && !c.leaving() && c.active(), "back in the game: delay cancelled");
        s = c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 2000 + kDelay);
        expect(s.kind == GameStep::Kind::None, "a new delay starts on the next leave");
    }

    // The user changed something during the game: that part is not restored.
    {
        GameModeController c;
        c.update(true, cur(PowerMode::Balanced, kQuiet), targets, 0);
        c.update(false, cur(PowerMode::BestPerformance, kBalanced), targets, 1000);
        const auto s = c.update(false, cur(PowerMode::BestPerformance, kBalanced), targets, 1000 + kDelay);
        expect(s.kind == GameStep::Kind::Restore && s.setPower == PowerMode::Balanced && !s.setProfile,
               "profile changed by the user: only power restored");
    }
    {
        GameModeController c;
        c.update(true, cur(PowerMode::Balanced, kQuiet), targets, 0);
        const auto s = c.restoreNow(cur(PowerMode::BestEfficiency, kGMode));
        expect(s.kind == GameStep::Kind::Restore && !s.setPower && s.setProfile == kQuiet,
               "power changed by the user: only profile restored");
    }

    // Already at the targets: nothing changed, nothing restored, no record.
    {
        GameModeController c;
        auto s = c.update(true, cur(PowerMode::BestPerformance, kGMode), targets, 0);
        expect(s.kind == GameStep::Kind::Enter && !s.setPower && !s.setProfile && !s.savePending,
               "targets already set: no change");
        s = c.restoreNow(cur(PowerMode::BestPerformance, kGMode));
        expect(s.kind == GameStep::Kind::Restore && !s.setPower && !s.setProfile, "nothing to restore");
    }

    // Unknown values are left alone.
    {
        GameModeController c;
        const auto s = c.update(true, cur(PowerMode::Other, std::nullopt), targets, 0);
        expect(!s.setPower && !s.setProfile, "unknown power mode / no service: no change");
    }

    // "Leave alone" targets.
    {
        GameModeController c;
        GameTargets onlyPower;
        onlyPower.profile.reset();
        const auto s = c.update(true, cur(PowerMode::Balanced, kQuiet), onlyPower, 0);
        expect(s.setPower == PowerMode::BestPerformance && !s.setProfile, "profile target unset: power only");
    }

    // Resume after a crash: restored once no game is in front, after the delay.
    {
        GameModeController c;
        GamePending p;
        p.savedPower = PowerMode::Balanced;
        p.appliedPower = PowerMode::BestPerformance;
        p.savedProfile = kQuiet;
        p.appliedProfile = kGMode;
        c.resume(p);
        expect(c.active(), "resumed record is active");
        expect(c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, 0).kind == GameStep::Kind::None,
               "resume: delay first");
        const auto s = c.update(false, cur(PowerMode::BestPerformance, kGMode), targets, kDelay);
        expect(s.kind == GameStep::Kind::Restore && s.setPower == PowerMode::Balanced && s.setProfile == kQuiet,
               "resume: restored");
    }
    {
        GameModeController c;
        GamePending p;
        p.savedProfile = kQuiet;
        p.appliedProfile = kGMode;
        c.resume(p);
        const auto s = c.update(true, cur(PowerMode::Balanced, kGMode), targets, 0);
        expect(s.kind == GameStep::Kind::None && c.active(), "resume with a game in front: stays engaged");
    }

    if (failures == 0) std::puts("game_mode_test: all passed");
    return failures == 0 ? 0 : 1;
}
