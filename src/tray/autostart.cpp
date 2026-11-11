#include "tray/autostart.h"

#include <windows.h>

#include <string>

namespace myalien::tray {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValue[] = L"MyAlien";

std::wstring command() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return L"\"" + std::wstring(path, n) + L"\"";
}

}  // namespace

bool autostartEnabled() {
    wchar_t buf[MAX_PATH + 8] = {};
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, command().c_str()) == 0;  // points at this executable
}

bool setAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS r;
    if (enable) {
        const std::wstring cmd = command();
        r = cmd.empty() ? ERROR_INVALID_DATA
                        : RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                                         static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

}  // namespace myalien::tray
