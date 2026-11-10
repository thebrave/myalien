#include "core/error.h"

#include <format>

namespace myalien {

namespace {

const char* wbemMessage(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80041001: return "WMI: generic failure";
        case 0x80041002: return "WMI: not found";
        case 0x80041003: return "WMI: access denied (run elevated)";
        case 0x80041004: return "WMI: provider failure";
        case 0x80041008: return "WMI: invalid parameter";
        case 0x8004100C: return "WMI: not supported";
        case 0x80041010: return "WMI: invalid class";
        case 0x80041017: return "WMI: invalid query";
        case 0x8004102F: return "WMI: invalid method parameters";
        default: return nullptr;
    }
}

}  // namespace

HrError::HrError(HRESULT hr, const std::string& context)
    : std::runtime_error(std::format("{}: {} (0x{:08X})", context, hrMessage(hr),
                                     static_cast<unsigned long>(hr))),
      hr_(hr) {}

bool HrError::accessDenied() const noexcept {
    return hr_ == static_cast<HRESULT>(0x80041003L) || hr_ == E_ACCESSDENIED;
}

void throwHr(HRESULT hr, const char* context) { throw HrError(hr, context); }

void throwLastError(const char* context) { throwHr(HRESULT_FROM_WIN32(GetLastError()), context); }

std::string hrMessage(HRESULT hr) {
    if (const char* w = wbemMessage(hr)) return w;

    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    if (n == 0 || !buf) return "unknown error";
    std::wstring msg(buf, n);
    LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'.'))
        msg.pop_back();
    return toUtf8(msg);
}

std::string toUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace myalien
