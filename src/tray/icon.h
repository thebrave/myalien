#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>

namespace myalien::tray {

// Small square badge: a rounded rectangle in the profile's colour with a white letter
// (Q quiet, B balanced, P balanced-performance, L low-power, C custom, G G-Mode, ? unknown).
// `profile` empty = service unreachable (grey "?"). Caller owns the icon (DestroyIcon).
HICON makeProfileIcon(std::optional<uint8_t> profile, int sizePx);

}  // namespace myalien::tray
