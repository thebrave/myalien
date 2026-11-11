#include "tray/game_settings.h"

#include <windows.h>

namespace myalien::tray {

namespace {

constexpr wchar_t kKey[] = L"Software\\MyAlien\\GameMode";
constexpr wchar_t kPendingKey[] = L"Software\\MyAlien\\GameMode\\Pending";
constexpr DWORD kNone = 0xFFFFFFFF;  // "leave alone" / "not changed"

std::optional<DWORD> readDword(const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return v;
}

bool writeDword(HKEY k, const wchar_t* name, DWORD v) {
    return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
}

// Power modes are stored by name-stable numbers, not the enum's order.
DWORD powerCode(std::optional<PowerMode> m) {
    if (!m) return kNone;
    switch (*m) {
        case PowerMode::BestEfficiency: return 1;
        case PowerMode::Balanced: return 2;
        case PowerMode::BestPerformance: return 3;
        default: return kNone;
    }
}

std::optional<PowerMode> powerFromCode(std::optional<DWORD> c) {
    switch (c.value_or(kNone)) {
        case 1: return PowerMode::BestEfficiency;
        case 2: return PowerMode::Balanced;
        case 3: return PowerMode::BestPerformance;
        default: return std::nullopt;
    }
}

std::vector<std::wstring> readList(const wchar_t* name) {
    std::vector<std::wstring> out;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return out;
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &bytes) != ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

bool writeList(HKEY k, const wchar_t* name, const std::vector<std::wstring>& items) {
    std::wstring buf;
    for (const auto& s : items)
        if (!s.empty()) buf += s + L'\0';
    buf += L'\0';
    return RegSetValueExW(k, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buf.data()),
                          static_cast<DWORD>(buf.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

DWORD profileCode(std::optional<uint8_t> p) { return p ? *p : kNone; }

std::optional<uint8_t> profileFromCode(std::optional<DWORD> c) {
    if (!c || *c > 0xFF) return std::nullopt;
    return static_cast<uint8_t>(*c);
}

}  // namespace

GameSettings loadGameSettings() {
    GameSettings s;
    s.enabled = readDword(kKey, L"Enabled").value_or(0) != 0;
    s.onlyOnAc = readDword(kKey, L"OnlyOnAc").value_or(1) != 0;
    s.notified = readDword(kKey, L"Notified").value_or(0) != 0;
    if (const auto v = readDword(kKey, L"PowerMode")) s.targets.power = powerFromCode(v);
    if (const auto v = readDword(kKey, L"Profile")) s.targets.profile = profileFromCode(v);
    s.folders = readList(L"Folders");
    s.excluded = readList(L"Excluded");
    return s;
}

bool saveGameSettings(const GameSettings& s) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    const bool ok = writeDword(k, L"Enabled", s.enabled) && writeDword(k, L"OnlyOnAc", s.onlyOnAc) &&
                    writeDword(k, L"Notified", s.notified) && writeDword(k, L"PowerMode", powerCode(s.targets.power)) &&
                    writeDword(k, L"Profile", profileCode(s.targets.profile)) && writeList(k, L"Folders", s.folders) &&
                    writeList(k, L"Excluded", s.excluded);
    RegCloseKey(k);
    return ok;
}

std::optional<GamePending> loadGamePending() {
    if (!readDword(kPendingKey, L"Active").value_or(0)) return std::nullopt;
    GamePending p;
    p.savedPower = powerFromCode(readDword(kPendingKey, L"SavedPower"));
    p.appliedPower = powerFromCode(readDword(kPendingKey, L"AppliedPower"));
    p.savedProfile = profileFromCode(readDword(kPendingKey, L"SavedProfile"));
    p.appliedProfile = profileFromCode(readDword(kPendingKey, L"AppliedProfile"));
    return p;
}

bool saveGamePending(const GamePending& p) {
    HKEY k = nullptr;
    // Not volatile: it must survive a crash or a reboot in the middle of a game.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPendingKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = writeDword(k, L"SavedPower", powerCode(p.savedPower)) &&
                    writeDword(k, L"AppliedPower", powerCode(p.appliedPower)) &&
                    writeDword(k, L"SavedProfile", profileCode(p.savedProfile)) &&
                    writeDword(k, L"AppliedProfile", profileCode(p.appliedProfile)) && writeDword(k, L"Active", 1);
    RegCloseKey(k);
    return ok;
}

void clearGamePending() { RegDeleteKeyW(HKEY_CURRENT_USER, kPendingKey); }

}  // namespace myalien::tray
