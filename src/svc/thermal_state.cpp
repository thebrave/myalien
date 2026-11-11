#include "svc/thermal_state.h"

#include "core/actions.h"
#include "core/error.h"

#include <windows.h>

#include <format>

namespace myalien::svc {

ThermalState readThermalState(Awcc& awcc) {
    ThermalState s;
    s.profile = awcc.currentProfile();
    for (const auto& f : awcc.caps().fans)
        if (const auto b = awcc.fan(f.id).boost) s.boosts[f.id] = *b;
    return s;
}

std::string describe(const ThermalState& s) {
    std::string r = std::format("{} (0x{:02X})", profileName(s.profile), s.profile);
    if (s.profile == profile::Custom) {
        r += ", boost";
        for (const auto& [id, b] : s.boosts) r += std::format(" {} {}", fanName(id), b);
    }
    return r;
}

bool sameSetting(const ThermalState& a, const ThermalState& b) {
    if (a.profile != b.profile) return false;
    return a.profile != profile::Custom || a.boosts == b.boosts;
}

// ---------------------------------------------------------------------------------------------
// state.ini
//   [thermal]
//   profile=163            (decimal profile id; absent = nothing chosen through MyAlien yet)
//   boosts=50:30,51:30     (fan id:boost, only meaningful in Custom)
//   overridden=0

namespace {
constexpr wchar_t kSection[] = L"thermal";
}

DesiredState loadDesired(const std::wstring& path) {
    DesiredState d;
    const int profile = static_cast<int>(GetPrivateProfileIntW(kSection, L"profile", -1, path.c_str()));
    if (profile < 0 || profile > 0xFF) return d;

    ThermalState t;
    t.profile = static_cast<uint8_t>(profile);
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(kSection, L"boosts", L"", buf, 256, path.c_str());
    // Parse "id:boost" pairs; anything malformed is ignored.
    std::wstring s(buf);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(L',', pos);
        if (end == std::wstring::npos) end = s.size();
        const std::wstring item = s.substr(pos, end - pos);
        const size_t colon = item.find(L':');
        if (colon != std::wstring::npos) {
            const unsigned long id = std::wcstoul(item.substr(0, colon).c_str(), nullptr, 10);
            const unsigned long b = std::wcstoul(item.substr(colon + 1).c_str(), nullptr, 10);
            if (id <= 0xFF && b <= kMaxFanBoost) t.boosts[static_cast<uint8_t>(id)] = b;
        }
        pos = end + 1;
    }
    d.thermal = t;
    d.overridden = GetPrivateProfileIntW(kSection, L"overridden", 0, path.c_str()) != 0;
    return d;
}

void saveDesired(const std::wstring& path, const DesiredState& d) {
    if (!d.thermal) {
        WritePrivateProfileStringW(kSection, nullptr, nullptr, path.c_str());  // delete section
        return;
    }
    std::wstring boosts;
    for (const auto& [id, b] : d.thermal->boosts)
        boosts += (boosts.empty() ? L"" : L",") + std::to_wstring(id) + L":" + std::to_wstring(b);
    WritePrivateProfileStringW(kSection, L"profile", std::to_wstring(d.thermal->profile).c_str(), path.c_str());
    WritePrivateProfileStringW(kSection, L"boosts", boosts.c_str(), path.c_str());
    WritePrivateProfileStringW(kSection, L"overridden", d.overridden ? L"1" : L"0", path.c_str());
}

std::wstring defaultStatePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash) + L"\\state.ini";
}

}  // namespace myalien::svc
