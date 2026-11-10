#pragma once

// Local IPC between the MyAlien service and its clients (CLI, tray).
//
// Transport: one named pipe, \\.\pipe\MyAlien, message mode, local clients only.
// One request per connection: the client writes one message, the server answers with one
// message and the client disconnects.
//
// Request : a single UTF-8 line, e.g. "PING", "STATUS".
// Response: first line "OK" or "ERR <message>", then an optional body (UTF-8, request-specific).
//
// The client only talks to a pipe whose server process is the running MyAlien service
// (GetNamedPipeServerProcessId vs. the service PID), and connects with SECURITY_IDENTIFICATION
// so a rogue server cannot impersonate the caller.

#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace myalien::ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\MyAlien";
inline constexpr wchar_t kServiceName[] = L"MyAlien";
inline constexpr size_t kMaxRequest = 4 * 1024;
inline constexpr size_t kMaxResponse = 256 * 1024;

struct Response {
    bool ok = false;
    std::string error;  // when !ok
    std::string body;
};

std::string makeOk(std::string_view body = {});
std::string makeError(std::string_view message);
Response parseResponse(std::string_view message);

enum class CallError {
    NotRunning,       // no pipe / service not running
    UntrustedServer,  // pipe owned by another process than the MyAlien service
    Timeout,
    Io,
};
const char* toString(CallError e);

struct CallResult {
    std::optional<Response> response;
    CallError error = CallError::Io;
};

// Sends one request and waits for the answer (overall timeout per I/O step).
// Developer escape hatch: MYALIEN_TRUST_ANY_SERVER=1 skips the server identity check
// (for `myalien-svc --console`, which is not the registered service).
CallResult call(std::string_view request, DWORD timeoutMs = 5000);

}  // namespace myalien::ipc
