// Named-pipe server: one instance, one request per connection, reused handle (no gap in which
// another process could create the pipe name).

#include "svc/pipe_server.h"

#include "core/error.h"
#include "core/ipc.h"
#include "core/snapshot.h"
#include "core/wmi.h"
#include "svc/log.h"

#include <sddl.h>

#include <memory>
#include <string>

namespace myalien::svc {

namespace {

// SYSTEM and Administrators: full; interactive users: read/write (connect + exchange messages).
// Protected DACL (P): nothing inherited.
constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
constexpr DWORD kIoTimeoutMs = 3000;

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreer {
    void operator()(void* p) const noexcept { LocalFree(p); }
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

// Request dispatch. Read-only in this increment.
//   PING          -> service name and version
//   STATUS        -> snapshot: live thermal + battery, cached charge settings (with their age)
//   STATUS FRESH  -> same, re-reading the charge settings first (several seconds)
std::string handleRequest(const std::string& raw, ServiceContext& ctx) {
    const std::string req = trim(raw);
    if (req == "PING") return ipc::makeOk(std::format("service=MyAlien\nversion={}\n", MYALIEN_VERSION));
    if (req == "STATUS" || req == "STATUS FRESH") {
        if (req == "STATUS FRESH") ctx.charge.refresh();
        SnapshotTiming t;
        Snapshot s = collectSnapshot({.charge = false, .biosMirror = false}, &t);
        ctx.charge.apply(s);
        const double total = t.systemMs + t.thermalMs + t.batteryMs;
        if (total > 1000)
            logf("slow STATUS: system {:.0f} ms, thermal {:.0f} ms ({} AWCC calls, {:.0f} ms inside), battery {:.0f} ms",
                 t.systemMs, t.thermalMs, t.awccCalls, t.awccCallMs, t.batteryMs);
        return ipc::makeOk(encodeSnapshot(s));
    }
    return ipc::makeError("unknown request");
}

enum class Io { Done, MoreData, Stopped, Timeout, Failed };

// Completes an overlapped ReadFile/WriteFile/ConnectNamedPipe started with `started`, waiting
// at most timeoutMs and waking up early when stopEvent is signalled.
Io complete(HANDLE pipe, OVERLAPPED& ov, BOOL started, HANDLE stopEvent, DWORD timeoutMs, DWORD& bytes) {
    bytes = 0;
    if (!started) {
        const DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            GetOverlappedResult(pipe, &ov, &bytes, FALSE);
            return Io::MoreData;
        }
        if (err != ERROR_IO_PENDING) return Io::Failed;

        const HANDLE handles[] = {ov.hEvent, stopEvent};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &bytes, TRUE);
            return w == WAIT_OBJECT_0 + 1 ? Io::Stopped : (w == WAIT_TIMEOUT ? Io::Timeout : Io::Failed);
        }
    }
    if (GetOverlappedResult(pipe, &ov, &bytes, FALSE)) return Io::Done;
    return GetLastError() == ERROR_MORE_DATA ? Io::MoreData : Io::Failed;
}

// Serves one connected client. Returns false when the server should stop.
bool serveClient(HANDLE pipe, HANDLE ioEvent, ServiceContext& ctx) {
    const HANDLE stopEvent = ctx.stopEvent;
    // Read the request: one message, possibly in several chunks, bounded.
    std::string request;
    char buf[1024];
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        DWORD bytes = 0;
        const Io r = complete(pipe, ov, ReadFile(pipe, buf, sizeof(buf), nullptr, &ov), stopEvent, kIoTimeoutMs, bytes);
        if (r == Io::Stopped) return false;
        if (r != Io::Done && r != Io::MoreData) return true;  // client went away or timed out
        request.append(buf, bytes);
        if (request.size() > ipc::kMaxRequest) {
            logLine("pipe: request too large, dropped");
            return true;
        }
        if (r == Io::Done) break;
    }

    std::string response;
    try {
        response = handleRequest(request, ctx);
    } catch (const std::exception& e) {
        response = ipc::makeError(e.what());
    }
    if (response.size() > ipc::kMaxResponse) response = ipc::makeError("response too large");

    {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        DWORD bytes = 0;
        const BOOL started = WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), nullptr, &ov);
        const Io r = complete(pipe, ov, started, stopEvent, kIoTimeoutMs, bytes);
        if (r == Io::Stopped) return false;
        if (r != Io::Done) return true;
    }

    // Let the client read the answer and close its end before disconnecting (DisconnectNamedPipe
    // discards unread data). The read completes with ERROR_BROKEN_PIPE once the client is gone.
    {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        DWORD bytes = 0;
        char drain[16];
        const BOOL started = ReadFile(pipe, drain, sizeof(drain), nullptr, &ov);
        if (complete(pipe, ov, started, stopEvent, kIoTimeoutMs, bytes) == Io::Stopped) return false;
    }
    return true;
}

}  // namespace

DWORD runPipeServer(ServiceContext& ctx) {
    const HANDLE stopEvent = ctx.stopEvent;
    wmi::ComApartment com;  // snapshots use WMI on this thread

    PSECURITY_DESCRIPTOR sdRaw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &sdRaw, nullptr)) {
        const DWORD err = GetLastError();
        logf("pipe: cannot build security descriptor (error {})", err);
        return err;
    }
    std::unique_ptr<void, LocalFreer> sd(sdRaw);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd.get(), FALSE};

    // FILE_FLAG_FIRST_PIPE_INSTANCE: fail if another process already owns the name (squatting).
    UniqueHandle pipe(CreateNamedPipeW(
        ipc::kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(ipc::kMaxResponse), static_cast<DWORD>(ipc::kMaxRequest), 0, &sa));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        logf("pipe: CreateNamedPipe failed (error {}){}", err,
             err == ERROR_ACCESS_DENIED ? " - pipe name already in use by another process" : "");
        return err;
    }
    UniqueHandle ioEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ioEvent) return GetLastError();

    logLine("pipe: listening on \\\\.\\pipe\\MyAlien");
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent.get();
        ResetEvent(ioEvent.get());
        bool connected = false;
        const BOOL started = ConnectNamedPipe(pipe.get(), &ov);
        if (!started && GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = true;  // client connected between CreateNamedPipe/Disconnect and Connect
        } else {
            DWORD bytes = 0;
            const Io r = complete(pipe.get(), ov, started, stopEvent, INFINITE, bytes);
            if (r == Io::Stopped) break;
            connected = (r == Io::Done);
        }

        if (connected && !serveClient(pipe.get(), ioEvent.get(), ctx)) {
            DisconnectNamedPipe(pipe.get());
            break;
        }
        DisconnectNamedPipe(pipe.get());
        // Back off briefly after a failed connect so an error cannot turn into a busy loop.
        if (WaitForSingleObject(stopEvent, connected ? 0 : 200) == WAIT_OBJECT_0) break;
    }
    logLine("pipe: stopped");
    return 0;
}

}  // namespace myalien::svc
