#pragma once

// Write operations shared by the CLI (elevated, run in-process) and the service (run on behalf of
// a pipe client). Each action validates its input against what the firmware reports, writes,
// reads the result back from the firmware, and returns user-facing output lines plus an exit code.
// Callers must have COM initialised on the thread.
//
// Inputs are strings as typed by the user (profile, fan and charge-mode names): the same
// validation therefore applies whether the request comes from the command line or the pipe.

#include "core/charge.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myalien {

namespace exitcode {
inline constexpr int Ok = 0;
inline constexpr int Error = 1;       // precondition not met, firmware/WMI failure
inline constexpr int Usage = 2;       // invalid argument
inline constexpr int NotApplied = 3;  // accepted but the firmware does not report the requested state
}  // namespace exitcode

struct ActionLine {
    bool error = false;  // stderr when printed by the CLI
    std::string text;
};

struct ActionOutput {
    int exitCode = exitcode::Ok;
    std::vector<ActionLine> lines;

    void out(std::string text) { lines.push_back({false, std::move(text)}); }
    void err(std::string text) { lines.push_back({true, std::move(text)}); }
    ActionOutput& fail(int code, std::string text) {
        exitCode = code;
        err(std::move(text));
        return *this;
    }
};

// Pipe encoding of an ActionOutput: "exit=N" then one "o:<text>" / "e:<text>" line per output line.
std::string encodeOutput(const ActionOutput& o);
ActionOutput decodeOutput(std::string_view body);  // throws std::runtime_error when malformed

// --- names -----------------------------------------------------------------------------------
// "balanced", "quiet", ... or a hex ID such as "0xA0".
std::optional<uint8_t> parseProfileName(std::string_view name);
// "Balanced (0xA0)  -> balanced"
std::string describeProfile(uint8_t id);

// --- actions ---------------------------------------------------------------------------------
// All refuse to write on anything but a Dell G15 5520.

ActionOutput actSetProfile(std::string_view profileName);
ActionOutput actSetGMode(bool on, std::string_view profileWhenOff = "balanced");
// fans: "cpu" | "gpu" | "all"; boost: decimal 0..100. Requires the Custom profile.
ActionOutput actSetFanBoost(std::string_view fans, std::string_view boost);

// mode: adaptive | standard | express | primacuse | ac | custom (any case, or BIOS spelling);
// start/stop only with custom. On success, `after` holds the settings read back.
struct ChargeActionResult {
    ActionOutput output;
    std::optional<ChargeSettings> after;
};
ChargeActionResult actSetChargeMode(std::string_view mode, std::optional<std::pair<std::string, std::string>> range);

}  // namespace myalien
