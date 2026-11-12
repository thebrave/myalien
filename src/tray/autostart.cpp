#include "tray/autostart.h"

#include <windows.h>

#include <string>

namespace myalien::tray {

// Two cases:
//  * Installed copy: the installer registers the tray for every user under HKLM\...\Run. Each
//    user turns it off or on with the same per-user switch Task Manager's Startup page uses
//    (HKCU\...\Explorer\StartupApproved\Run), which needs no elevation.
//  * Any other copy (a build tree): a per-user HKCU\...\Run entry pointing at this executable.

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValue[] = L"MyAlien";

std::wstring command() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return L"\"" + std::wstring(path, n) + L"\"";
}

bool runEntryIsThis(HKEY root) {
    wchar_t buf[MAX_PATH + 8] = {};
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(root, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, command().c_str()) == 0;
}

// StartupApproved: 12 bytes; first byte 2 (or no value) = enabled, 3 = disabled by the user.
bool approved() {
    BYTE data[12] = {};
    DWORD bytes = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, data, &bytes) != ERROR_SUCCESS)
        return true;
    return (data[0] & 1) == 0;
}

bool setApproved(bool enable) {
    BYTE data[12] = {static_cast<BYTE>(enable ? 2 : 3)};
    if (!enable) {  // Task Manager stores when it was disabled
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        memcpy(data + 4, &now, sizeof(now));
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = RegSetValueExW(key, kValue, 0, REG_BINARY, data, sizeof(data)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

}  // namespace

bool autostartEnabled() {
    if (runEntryIsThis(HKEY_LOCAL_MACHINE)) return approved();
    return runEntryIsThis(HKEY_CURRENT_USER);
}

bool setAutostart(bool enable) {
    if (runEntryIsThis(HKEY_LOCAL_MACHINE)) return setApproved(enable);

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
