#include "core/wmi.h"

#include "core/error.h"

#include <cwchar>

namespace myalien::wmi {

// ---------------------------------------------------------------------------------------------
ComApartment::ComApartment() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        // Thread already in an STA: usable for WMI, but not ours to uninitialise.
    } else {
        check(hr, "CoInitializeEx");
        uninitialize_ = true;
    }

    const HRESULT sec = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(sec) && sec != RPC_E_TOO_LATE) {
        if (uninitialize_) CoUninitialize();
        throwHr(sec, "CoInitializeSecurity");
    }
}

ComApartment::~ComApartment() {
    if (uninitialize_) CoUninitialize();
}

// ---------------------------------------------------------------------------------------------
Bstr::Bstr(const wchar_t* s) : b_(SysAllocString(s)) {
    if (!b_ && s && *s) throw std::bad_alloc();
}

Bstr::~Bstr() { SysFreeString(b_); }

// ---------------------------------------------------------------------------------------------
namespace {

// Copies element `index` of a SAFEARRAY variant into a scalar VARIANT.
bool arrayElement(const VARIANT& v, LONG index, VARIANT& elem) {
    VariantInit(&elem);
    if (!(v.vt & VT_ARRAY) || !v.parray) return false;
    const VARTYPE et = static_cast<VARTYPE>(v.vt & VT_TYPEMASK);
    elem.vt = et;
    elem.llVal = 0;
    void* dst = (et == VT_BSTR) ? static_cast<void*>(&elem.bstrVal) : static_cast<void*>(&elem.llVal);
    if (FAILED(SafeArrayGetElement(v.parray, &index, dst))) {
        elem.vt = VT_EMPTY;
        return false;
    }
    return true;
}

bool arrayBounds(const VARIANT& v, LONG& lb, LONG& ub) {
    if (!(v.vt & VT_ARRAY) || !v.parray) return false;
    return SUCCEEDED(SafeArrayGetLBound(v.parray, 1, &lb)) && SUCCEEDED(SafeArrayGetUBound(v.parray, 1, &ub)) &&
           ub >= lb;
}

}  // namespace

std::optional<int64_t> toInt(const VARIANT& v) {
    switch (v.vt) {
        case VT_I1: return v.cVal;
        case VT_UI1: return v.bVal;
        case VT_I2: return v.iVal;
        case VT_UI2: return v.uiVal;
        case VT_I4: return v.lVal;
        case VT_UI4: return v.ulVal;
        case VT_INT: return v.intVal;
        case VT_UINT: return v.uintVal;
        case VT_I8: return v.llVal;
        case VT_UI8: return static_cast<int64_t>(v.ullVal);
        case VT_BOOL: return v.boolVal != VARIANT_FALSE ? 1 : 0;
        case VT_BSTR: {  // WMI returns 64-bit integers as strings
            if (!v.bstrVal) return std::nullopt;
            wchar_t* end = nullptr;
            const long long x = std::wcstoll(v.bstrVal, &end, 0);
            if (end == v.bstrVal) return std::nullopt;
            return x;
        }
        default: break;
    }
    LONG lb = 0, ub = -1;
    if (arrayBounds(v, lb, ub)) {
        VARIANT e;
        if (arrayElement(v, lb, e)) {
            auto r = toInt(e);
            VariantClear(&e);
            return r;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> toString(const VARIANT& v) {
    if (v.vt == VT_BSTR) return v.bstrVal ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : std::wstring();
    if (v.vt == VT_EMPTY || v.vt == VT_NULL) return std::nullopt;
    LONG lb = 0, ub = -1;
    if (arrayBounds(v, lb, ub)) {
        VARIANT e;
        if (arrayElement(v, lb, e)) {
            auto r = toString(e);
            VariantClear(&e);
            return r;
        }
        return std::nullopt;
    }
    if (auto i = toInt(v)) return std::to_wstring(*i);
    return std::nullopt;
}

std::vector<std::wstring> toStringArray(const VARIANT& v) {
    std::vector<std::wstring> out;
    LONG lb = 0, ub = -1;
    if (arrayBounds(v, lb, ub)) {
        for (LONG i = lb; i <= ub; ++i) {
            VARIANT e;
            if (arrayElement(v, i, e)) {
                if (auto s = toString(e)) out.push_back(std::move(*s));
                VariantClear(&e);
            }
        }
    } else if (auto s = toString(v)) {
        out.push_back(std::move(*s));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
namespace {
template <typename F>
auto readProp(IWbemClassObject* p, const wchar_t* name, F&& conv) -> decltype(conv(std::declval<const VARIANT&>())) {
    ScopedVariant v;
    if (!p || FAILED(p->Get(name, 0, v.get(), nullptr, nullptr))) return {};
    return conv(v.ref());
}
}  // namespace

std::optional<int64_t> Object::getInt(const wchar_t* name) const { return readProp(p_.Get(), name, toInt); }

std::optional<bool> Object::getBool(const wchar_t* name) const {
    auto i = getInt(name);
    if (!i) return std::nullopt;
    return *i != 0;
}

std::optional<std::wstring> Object::getString(const wchar_t* name) const {
    return readProp(p_.Get(), name, toString);
}

std::vector<std::wstring> Object::getStringArray(const wchar_t* name) const {
    return readProp(p_.Get(), name, toStringArray);
}

// ---------------------------------------------------------------------------------------------
Namespace Namespace::connect(const wchar_t* path) {
    ComPtr<IWbemLocator> locator;
    check(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)),
          "CoCreateInstance(WbemLocator)");

    Namespace ns;
    check(locator->ConnectServer(Bstr(path), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &ns.svc_),
          "IWbemLocator::ConnectServer");
    check(CoSetProxyBlanket(ns.svc_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE),
          "CoSetProxyBlanket");
    return ns;
}

std::vector<Object> Namespace::query(const std::wstring& wql) const {
    ComPtr<IEnumWbemClassObject> en;
    check(svc_->ExecQuery(Bstr(L"WQL"), Bstr(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                          &en),
          "IWbemServices::ExecQuery");

    std::vector<Object> out;
    for (;;) {
        ComPtr<IWbemClassObject> obj;
        ULONG n = 0;
        const HRESULT hr = en->Next(WBEM_INFINITE, 1, &obj, &n);
        if (FAILED(hr)) throwHr(hr, "IEnumWbemClassObject::Next");
        if (n == 0) break;
        out.emplace_back(std::move(obj));
    }
    return out;
}

Object Namespace::getClass(const wchar_t* className) const {
    ComPtr<IWbemClassObject> cls;
    check(svc_->GetObject(Bstr(className), 0, nullptr, &cls, nullptr), "IWbemServices::GetObject");
    return Object(std::move(cls));
}

Object Namespace::execMethod(const std::wstring& objectPath, const wchar_t* method,
                             IWbemClassObject* inParams) const {
    ComPtr<IWbemClassObject> out;
    check(svc_->ExecMethod(Bstr(objectPath), Bstr(method), 0, nullptr, inParams, &out, nullptr),
          "IWbemServices::ExecMethod");
    return Object(std::move(out));
}

}  // namespace myalien::wmi
