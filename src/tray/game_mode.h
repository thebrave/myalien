#pragma once

// Game mode decisions (docs/game-mode.md): when to switch the Windows power mode and the thermal
// profile, and what to restore. Pure logic: no Win32, no I/O, so it is unit-tested; the tray
// feeds it inputs and carries out the returned step.

#include "core/system.h"

#include <cstdint>
#include <optional>

namespace myalien::tray {

// What game mode sets while a game is in front; nullopt = leave that part alone.
struct GameTargets {
    std::optional<PowerMode> power = PowerMode::BestPerformance;
    std::optional<uint8_t> profile = uint8_t{0xAB};  // G-Mode
};

// Values read just now. profile is nullopt when the thermal state is unknown (no service).
struct GameCurrent {
    PowerMode power = PowerMode::Unknown;
    std::optional<uint8_t> profile;
};

// What game mode changed, so it can be undone. Persisted while a game is active, so a tray
// crash or logoff can still restore.
struct GamePending {
    std::optional<PowerMode> savedPower, appliedPower;
    std::optional<uint8_t> savedProfile, appliedProfile;

    bool changedSomething() const { return appliedPower.has_value() || appliedProfile.has_value(); }
};

struct GameStep {
    enum class Kind { None, Enter, Restore } kind = Kind::None;
    std::optional<PowerMode> setPower;
    std::optional<uint8_t> setProfile;
    bool savePending = false;   // write the pending record (before carrying out setPower/setProfile)
    bool clearPending = false;  // delete it
};

class GameModeController {
public:
    static constexpr uint64_t kLeaveDelayMs = 8000;  // alt-tab, launcher overlays, taskbar clicks

    // gameInFront: a game owns the foreground and game mode may act (enabled, power source OK).
    GameStep update(bool gameInFront, const GameCurrent& now, const GameTargets& targets, uint64_t nowMs);

    // Restore right away (game mode turned off, tray exiting).
    GameStep restoreNow(const GameCurrent& now);

    // A pending record found at start: restore it once no game is in front (after the delay).
    void resume(const GamePending& p) { pending_ = p; leaveAt_.reset(); }

    bool active() const { return pending_.has_value(); }
    bool leaving() const { return leaveAt_.has_value(); }
    const std::optional<GamePending>& pending() const { return pending_; }

private:
    std::optional<GamePending> pending_;  // set while game mode is engaged
    std::optional<uint64_t> leaveAt_;
};

}  // namespace myalien::tray
