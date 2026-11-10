#include "svc/log.h"

#include "core/error.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace myalien::svc {

namespace {

std::mutex g_mutex;
LogTarget g_target = LogTarget::Console;
std::wstring g_file;  // empty: file logging unavailable

constexpr LONGLONG kMaxLogBytes = 1024 * 1024;

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

void rotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_file.c_str(), GetFileExInfoStandard, &fad)) return;
    const LONGLONG size = (static_cast<LONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size < kMaxLogBytes) return;
    MoveFileExW(g_file.c_str(), (g_file + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string timestamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                       t.wSecond, t.wMilliseconds);
}

}  // namespace

void initLog(LogTarget target) {
    std::lock_guard lock(g_mutex);
    g_target = target;
    if (target != LogTarget::File) return;
    const std::wstring dir = exeDirectory();
    if (dir.empty()) return;
    const std::wstring logDir = dir + L"\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);  // inherits the install directory's ACL
    g_file = logDir + L"\\service.log";
}

void logLine(std::string_view message) {
    const std::string line = std::format("{} [{}] {}\n", timestamp(), GetCurrentThreadId(), message);
    std::lock_guard lock(g_mutex);
    if (g_target == LogTarget::Console) {
        std::fputs(line.c_str(), stderr);
        return;
    }
    if (g_file.empty()) {
        OutputDebugStringA(line.c_str());
        return;
    }
    rotateIfNeeded();
    HANDLE h = CreateFileW(g_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        OutputDebugStringA(line.c_str());
        return;
    }
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
}

}  // namespace myalien::svc
