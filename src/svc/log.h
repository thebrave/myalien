#pragma once

// Minimal service log.
//  * Service mode: <install dir>\logs\service.log (admin-only location under Program Files),
//    rotated to service.log.1 above 1 MB.
//  * Console mode: stderr only.

#include <format>
#include <string>
#include <string_view>

namespace myalien::svc {

enum class LogTarget { Console, File };

void initLog(LogTarget target);
void logLine(std::string_view message);

template <typename... Args>
void logf(std::format_string<Args...> fmt, Args&&... args) {
    logLine(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace myalien::svc
