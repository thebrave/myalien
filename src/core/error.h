#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace myalien {

// Exception carrying an HRESULT (COM / WMI / Win32 errors).
class HrError : public std::runtime_error {
public:
    HrError(HRESULT hr, const std::string& context);
    HRESULT hr() const noexcept { return hr_; }
    bool accessDenied() const noexcept;

private:
    HRESULT hr_;
};

[[noreturn]] void throwHr(HRESULT hr, const char* context);
[[noreturn]] void throwLastError(const char* context);

inline void check(HRESULT hr, const char* context) {
    if (FAILED(hr)) throwHr(hr, context);
}

// Human readable text for an HRESULT, including common WMI (WBEM_E_*) codes.
std::string hrMessage(HRESULT hr);

std::string toUtf8(std::wstring_view s);
std::wstring toWide(std::string_view s);

}  // namespace myalien
