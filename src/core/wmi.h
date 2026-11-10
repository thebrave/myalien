#pragma once

// Thin RAII layer over COM WMI (IWbemServices). Only what MyAlien needs:
// connect to a namespace, run WQL queries, read properties, execute methods.

#include <windows.h>
#include <objbase.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myalien::wmi {

using Microsoft::WRL::ComPtr;

// Initialises COM (MTA) and process-wide COM security for the lifetime of the object.
// Create exactly one, early, on the thread that uses WMI.
class ComApartment {
public:
    ComApartment();
    ~ComApartment();
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool uninitialize_ = false;
};

class Bstr {
public:
    explicit Bstr(const wchar_t* s);
    explicit Bstr(const std::wstring& s) : Bstr(s.c_str()) {}
    ~Bstr();
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const noexcept { return b_; }

private:
    BSTR b_;
};

class ScopedVariant {
public:
    ScopedVariant() noexcept { VariantInit(&v_); }
    ~ScopedVariant() { VariantClear(&v_); }
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;
    VARIANT* get() noexcept { return &v_; }
    const VARIANT& ref() const noexcept { return v_; }

private:
    VARIANT v_;
};

// Variant conversions. Arrays yield their first element (Dell BIOS attributes
// sometimes expose scalars as one-element arrays).
std::optional<int64_t> toInt(const VARIANT& v);
std::optional<std::wstring> toString(const VARIANT& v);
std::vector<std::wstring> toStringArray(const VARIANT& v);

class Object {
public:
    Object() = default;
    explicit Object(ComPtr<IWbemClassObject> p) : p_(std::move(p)) {}

    IWbemClassObject* get() const noexcept { return p_.Get(); }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    std::optional<int64_t> getInt(const wchar_t* name) const;
    std::optional<bool> getBool(const wchar_t* name) const;
    std::optional<std::wstring> getString(const wchar_t* name) const;
    std::vector<std::wstring> getStringArray(const wchar_t* name) const;

private:
    ComPtr<IWbemClassObject> p_;
};

class Namespace {
public:
    // path e.g. L"ROOT\\WMI". Connecting usually succeeds without elevation;
    // access checks happen when reading instances / calling methods.
    static Namespace connect(const wchar_t* path);

    std::vector<Object> query(const std::wstring& wql) const;
    Object getClass(const wchar_t* className) const;
    Object execMethod(const std::wstring& objectPath, const wchar_t* method, IWbemClassObject* inParams) const;

private:
    ComPtr<IWbemServices> svc_;
};

}  // namespace myalien::wmi
