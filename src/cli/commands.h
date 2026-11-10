#pragma once

#include <span>
#include <string_view>

namespace myalien::cli {

// Returns a process exit code.
int cmdStatus(std::span<const std::wstring_view> args);

}  // namespace myalien::cli
