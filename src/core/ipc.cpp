#include "core/ipc.h"

#include <cstdlib>
#include <memory>
#include <type_traits>

namespace myalien::ipc {

std::string makeOk(std::string_view body) {
    std::string r = "OK\n";
    r += body;
    return r;
}

std::string makeError(std::string_view message) {
    std::string r = "ERR ";
    for (char c : message) r += (c == '\r' || c == '\n') ? ' ' : c;
    r += '\n';
    return r;
}

Response parseResponse(std::string_view m) {
    Response r;
    const size_t nl = m.find('\n');
    std::string_view first = m.substr(0, nl);
    if (!first.empty() && first.back() == '\r') first.remove_suffix(1);
    const std::string_view body = nl == std::string_view::npos ? std::string_view() : m.substr(nl + 1);
    if (first == "OK") {
        r.ok = true;
        r.body = body;
    } else if (first.starts_with("ERR")) {
        r.error = first.size() > 4 ? std::string(first.substr(4)) : std::string("error");
        r.body = body;
    } else {
        r.error = "malformed response";
    }
    return r;
}

const char* toString(CallError e) {
    switch (e) {
        case CallError::NotRunning: return "MyAlien service is not running";
        case CallError::UntrustedServer: return "pipe is not served by the MyAlien service";
        case CallError::Timeout: return "MyAlien service did not answer in time";
        default: return "I/O error talking to the MyAlien service";
    }
}

namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ScCloser {
    void operator()(SC_HANDLE h) const noexcept {
        if (h) CloseServiceHandle(h);
    }
};
using UniqueSc = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScCloser>;

std::optional<DWORD> servicePid() {
    UniqueSc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return std::nullopt;
    UniqueSc svc(OpenServiceW(scm.get(), kServiceName, SERVICE_QUERY_STATUS));
    if (!svc) return std::nullopt;
    SERVICE_STATUS_PROCESS sp{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&sp), sizeof(sp), &needed))
        return std::nullopt;
    if (sp.dwCurrentState != SERVICE_RUNNING || sp.dwProcessId == 0) return std::nullopt;
    return sp.dwProcessId;
}

bool trustAnyServer() {
    wchar_t buf[8] = {};
    const DWORD n = GetEnvironmentVariableW(L"MYALIEN_TRUST_ANY_SERVER", buf, 8);
    return n == 1 && buf[0] == L'1';
}

// Runs one overlapped I/O step with a timeout. Returns bytes transferred, or the error.
enum class IoResult { Ok, MoreData, Timeout, Failed };

IoResult waitIo(HANDLE pipe, OVERLAPPED& ov, BOOL started, DWORD timeoutMs, DWORD& bytes) {
    if (!started) {
        const DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            GetOverlappedResult(pipe, &ov, &bytes, FALSE);
            return IoResult::MoreData;
        }
        if (err != ERROR_IO_PENDING) return IoResult::Failed;
        if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &bytes, TRUE);
            return IoResult::Timeout;
        }
    }
    if (GetOverlappedResult(pipe, &ov, &bytes, FALSE)) return IoResult::Ok;
    return GetLastError() == ERROR_MORE_DATA ? IoResult::MoreData : IoResult::Failed;
}

}  // namespace

CallResult call(std::string_view request, DWORD timeoutMs) {
    CallResult result;

    if (!WaitNamedPipeW(kPipeName, timeoutMs)) {
        result.error = GetLastError() == ERROR_SEM_TIMEOUT ? CallError::Timeout : CallError::NotRunning;
        return result;
    }

    UniqueHandle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        result.error = GetLastError() == ERROR_PIPE_BUSY ? CallError::Timeout : CallError::NotRunning;
        return result;
    }

    if (!trustAnyServer()) {
        ULONG serverPid = 0;
        const auto expected = servicePid();
        if (!GetNamedPipeServerProcessId(pipe.get(), &serverPid) || !expected || serverPid != *expected) {
            result.error = CallError::UntrustedServer;
            return result;
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
        result.error = CallError::Io;
        return result;
    }

    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        result.error = CallError::Io;
        return result;
    }

    // Write the request.
    {
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        DWORD bytes = 0;
        const std::string req(request.substr(0, kMaxRequest));
        const BOOL started = WriteFile(pipe.get(), req.data(), static_cast<DWORD>(req.size()), nullptr, &ov);
        const auto r = waitIo(pipe.get(), ov, started, timeoutMs, bytes);
        if (r != IoResult::Ok || bytes != req.size()) {
            result.error = r == IoResult::Timeout ? CallError::Timeout : CallError::Io;
            return result;
        }
    }

    // Read the response message (possibly in several chunks).
    std::string message;
    char buf[8192];
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        ResetEvent(event.get());
        DWORD bytes = 0;
        const BOOL started = ReadFile(pipe.get(), buf, sizeof(buf), nullptr, &ov);
        const auto r = waitIo(pipe.get(), ov, started, timeoutMs, bytes);
        if (r == IoResult::Timeout) {
            result.error = CallError::Timeout;
            return result;
        }
        if (r == IoResult::Failed) {
            result.error = CallError::Io;
            return result;
        }
        message.append(buf, bytes);
        if (message.size() > kMaxResponse) {
            result.error = CallError::Io;
            return result;
        }
        if (r == IoResult::Ok) break;  // end of message
    }

    result.response = parseResponse(message);
    return result;
}

}  // namespace myalien::ipc
