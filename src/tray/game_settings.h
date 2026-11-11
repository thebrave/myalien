#pragma once

// Game mode settings and the pending-restore record, per user in HKCU\Software\MyAlien\GameMode.

#include "tray/game_mode.h"

#include <optional>

namespace myalien::tray {

struct GameSettings {
    bool enabled = false;  // opt-in
    GameTargets targets;   // default: Best performance + G-Mode
    bool onlyOnAc = true;  // G-Mode on battery drains it fast
    bool notified = false; // the one-time "game mode acted" notification was shown
};

GameSettings loadGameSettings();
bool saveGameSettings(const GameSettings& s);

std::optional<GamePending> loadGamePending();
bool saveGamePending(const GamePending& p);
void clearGamePending();

}  // namespace myalien::tray
