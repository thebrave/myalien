#include "core/system.h"

#include <windows.h>

#include <cstring>
#include <memory>
#include <type_traits>

namespace myalien {

namespace {

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

struct ScHandleCloser {
    void operator()(SC_HANDLE h) const noexcept {
        if (h) CloseServiceHandle(h);
    }
};
using UniqueScHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScHandleCloser>;

// Power overlay GUIDs (Settings > System > Power > Power mode).
constexpr GUID kOverlayBestEfficiency = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
constexpr GUID kOverlayBestPerformance = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
constexpr GUID kOverlayBalanced = {};

}  // namespace

bool MachineInfo::isG15_5520() const { return manufacturer == L"Dell Inc." && product == L"Dell G15 5520"; }

MachineInfo machineInfo() {
    constexpr wchar_t kBios[] = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    MachineInfo m;
    m.manufacturer = regString(HKEY_LOCAL_MACHINE, kBios, L"SystemManufacturer");
    m.product = regString(HKEY_LOCAL_MACHINE, kBios, L"SystemProductName");
    m.sku = regString(HKEY_LOCAL_MACHINE, kBios, L"SystemSKU");
    m.biosVersion = regString(HKEY_LOCAL_MACHINE, kBios, L"BIOSVersion");
    return m;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) != FALSE;
    CloseHandle(token);
    return ok && e.TokenIsElevated != 0;
}

const char* toString(ServiceState s) {
    switch (s) {
        case ServiceState::NotInstalled: return "not installed";
        case ServiceState::Stopped: return "stopped";
        case ServiceState::Running: return "running";
        case ServiceState::Transitioning: return "starting/stopping";
        default: return "unknown";
    }
}

ServiceState fusionServiceState() {
    UniqueScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return ServiceState::Unknown;
    UniqueScHandle svc(OpenServiceW(scm.get(), L"FusionService", SERVICE_QUERY_STATUS));
    if (!svc)
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::NotInstalled : ServiceState::Unknown;
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc.get(), &st)) return ServiceState::Unknown;
    switch (st.dwCurrentState) {
        case SERVICE_RUNNING: return ServiceState::Running;
        case SERVICE_STOPPED: return ServiceState::Stopped;
        default: return ServiceState::Transitioning;
    }
}

const char* toString(PowerMode m) {
    switch (m) {
        case PowerMode::BestEfficiency: return "Best power efficiency";
        case PowerMode::Balanced: return "Balanced";
        case PowerMode::BestPerformance: return "Best performance";
        case PowerMode::Other: return "Other";
        default: return "Unknown";
    }
}

PowerMode windowsPowerMode() {
    // PowerGetEffectiveOverlayScheme is exported by powrprof.dll but not declared in all SDKs.
    using Fn = DWORD(WINAPI*)(GUID*);
    HMODULE mod = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return PowerMode::Unknown;
    PowerMode result = PowerMode::Unknown;
    if (auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, "PowerGetEffectiveOverlayScheme"))) {
        GUID g{};
        if (fn(&g) == ERROR_SUCCESS) {
            if (IsEqualGUID(g, kOverlayBestEfficiency)) result = PowerMode::BestEfficiency;
            else if (IsEqualGUID(g, kOverlayBestPerformance)) result = PowerMode::BestPerformance;
            else if (IsEqualGUID(g, kOverlayBalanced)) result = PowerMode::Balanced;
            else result = PowerMode::Other;
        }
    }
    FreeLibrary(mod);
    return result;
}

std::optional<bool> onAcPower() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || s.ACLineStatus == 255) return std::nullopt;
    return s.ACLineStatus == 1;
}

}  // namespace myalien
