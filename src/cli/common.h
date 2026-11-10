#pragma once

// Helpers shared by the CLI commands that read or change the thermal state.

#include "core/awcc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/snapshot.h"

#include <utility>

namespace myalien::cli {

// Battery section of `status` (health, charge, charge mode).
void printBatterySection(const Snapshot& s);

// Snapshot for display, plus a label of where it came from:
//  * elevated            -> collected locally;
//  * not elevated        -> asked to the MyAlien service when it is running (charge settings
//                           from its cache, or re-read first when freshCharge is set);
//  * otherwise           -> collected locally (Dell sections will show access denied).
// Requires COM initialised on the calling thread.
std::pair<Snapshot, std::string> obtainSnapshot(const SnapshotParts& parts, bool freshCharge = false);

// "balanced", "quiet", ... or a hex ID such as "0xA0".
std::optional<uint8_t> parseProfile(std::wstring_view arg);

// "Balanced (0xA0)  -> balanced"
std::string describeProfile(uint8_t id);

// Current BIOS AWThermalManagement value, nullopt if unreadable.
std::optional<std::wstring> readAwThermal();

// Polls AWThermalManagement for up to ~2 s and prints it; flags a mismatch when the
// expected mirror of `profile` is known.
void printAwThermalAfterWrite(uint8_t profile);

// Checks that a firmware write may proceed (G15 5520, elevated). Prints why not and
// returns the exit code to use; nullopt when OK.
std::optional<int> refuseWrite(const char* what);

// Prints a note when Dell's FusionService (MyDell's backend) is running.
void noteMyDell();

}  // namespace myalien::cli
