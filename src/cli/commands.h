#pragma once

#include <span>
#include <string_view>

namespace myalien::cli {

// Returns a process exit code.
int cmdStatus(std::span<const std::wstring_view> args);
int cmdProfile(std::span<const std::wstring_view> args);
int cmdGMode(std::span<const std::wstring_view> args);
int cmdBattery(std::span<const std::wstring_view> args);
int cmdFan(std::span<const std::wstring_view> args);
int cmdGames(std::span<const std::wstring_view> args);
int cmdPowerMode(std::span<const std::wstring_view> args);

}  // namespace myalien::cli
