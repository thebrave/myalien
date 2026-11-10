#include "core/battery.h"

#include "core/error.h"

#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <batclass.h>

#include <memory>

namespace myalien {

namespace {

// GUID_DEVICE_BATTERY {72631E54-78A4-11D0-BCF7-00AA00B7B32A} (defined here to avoid initguid.h games).
constexpr GUID kGuidDeviceBattery = {0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct DevInfoCloser {
    void operator()(HDEVINFO h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
    }
};
using UniqueDevInfo = std::unique_ptr<void, DevInfoCloser>;

template <typename T>
bool queryInfo(HANDLE h, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level, T& out) {
    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = level;
    DWORD bytes = 0;
    return DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), &out, sizeof(out), &bytes, nullptr) &&
           bytes > 0;
}

std::wstring queryString(HANDLE h, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level) {
    wchar_t buf[256] = {};
    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = level;
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), buf, sizeof(buf) - sizeof(wchar_t),
                         &bytes, nullptr))
        return {};
    std::wstring s(buf, bytes / sizeof(wchar_t));
    while (!s.empty() && (s.back() == L'\0' || s.back() == L' ')) s.pop_back();
    return s;
}

std::optional<BatteryInfo> queryOne(const wchar_t* devicePath) {
    UniqueHandle h(CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return std::nullopt;

    ULONG wait = 0, tag = 0;
    DWORD bytes = 0;
    if (!DeviceIoControl(h.get(), IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &bytes, nullptr) ||
        tag == 0)
        return std::nullopt;

    BatteryInfo b;
    BATTERY_INFORMATION bi{};
    if (queryInfo(h.get(), tag, BatteryInformation, bi)) {
        b.relativeCapacity = (bi.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
        b.designCapacity = bi.DesignedCapacity;
        b.fullChargeCapacity = bi.FullChargedCapacity;
        if (bi.CycleCount != 0) b.cycleCount = bi.CycleCount;
        for (UCHAR c : bi.Chemistry)
            if (c >= 0x20 && c < 0x7F) b.chemistry.push_back(static_cast<char>(c));
        while (!b.chemistry.empty() && b.chemistry.back() == ' ') b.chemistry.pop_back();
    }

    ULONG tempDeciKelvin = 0;
    if (queryInfo(h.get(), tag, BatteryTemperature, tempDeciKelvin) && tempDeciKelvin != 0)
        b.temperatureC = tempDeciKelvin / 10.0 - 273.15;

    b.deviceName = queryString(h.get(), tag, BatteryDeviceName);
    b.manufacturer = queryString(h.get(), tag, BatteryManufactureName);
    b.serial = queryString(h.get(), tag, BatterySerialNumber);

    BATTERY_WAIT_STATUS ws{};
    ws.BatteryTag = tag;
    BATTERY_STATUS st{};
    if (DeviceIoControl(h.get(), IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof(ws), &st, sizeof(st), &bytes, nullptr)) {
        b.onLine = (st.PowerState & BATTERY_POWER_ON_LINE) != 0;
        b.charging = (st.PowerState & BATTERY_CHARGING) != 0;
        b.discharging = (st.PowerState & BATTERY_DISCHARGING) != 0;
        b.critical = (st.PowerState & BATTERY_CRITICAL) != 0;
        if (st.Capacity != BATTERY_UNKNOWN_CAPACITY) b.remainingCapacity = st.Capacity;
        if (st.Voltage != BATTERY_UNKNOWN_VOLTAGE) b.voltage_mV = st.Voltage;
        if (static_cast<ULONG>(st.Rate) != BATTERY_UNKNOWN_RATE) b.rate_mW = st.Rate;
    }
    return b;
}

}  // namespace

std::optional<double> BatteryInfo::healthPercent() const {
    if (relativeCapacity || designCapacity == 0 || fullChargeCapacity == 0) return std::nullopt;
    return 100.0 * fullChargeCapacity / designCapacity;
}

std::optional<double> BatteryInfo::chargePercent() const {
    if (!remainingCapacity || fullChargeCapacity == 0) return std::nullopt;
    return 100.0 * *remainingCapacity / fullChargeCapacity;
}

std::vector<BatteryInfo> queryBatteries() {
    std::vector<BatteryInfo> out;
    UniqueDevInfo dev(SetupDiGetClassDevsW(&kGuidDeviceBattery, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (dev.get() == INVALID_HANDLE_VALUE) throwLastError("SetupDiGetClassDevs(battery)");

    for (DWORD i = 0; i < 8; ++i) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(dev.get(), nullptr, &kGuidDeviceBattery, i, &did)) break;

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(dev.get(), &did, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;
        std::unique_ptr<BYTE[]> buf(new BYTE[needed]);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.get());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(dev.get(), &did, detail, needed, nullptr, nullptr)) continue;

        if (auto b = queryOne(detail->DevicePath)) out.push_back(std::move(*b));
    }
    return out;
}

}  // namespace myalien
