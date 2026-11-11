#include "svc/service.h"

#include "core/error.h"
#include "core/ipc.h"
#include "core/system.h"
#include "svc/log.h"
#include "svc/pipe_server.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

namespace myalien::svc {

namespace {

constexpr wchar_t kDisplayName[] = L"MyAlien thermal & battery service";
constexpr wchar_t kDescription[] =
    L"Thermal profiles, fans and battery charging for the Dell G15 5520 through Windows' ACPI-WMI interfaces. "
    L"Serves the MyAlien tray app and command line.";
constexpr wchar_t kExeName[] = L"myalien-svc.exe";

// ---------------------------------------------------------------------------------------------
// Service runtime

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
ServiceContext* g_ctx = nullptr;  // valid while the service / console server runs

// Runs the charge refresher, the thermal monitor and the pipe server until stop. Returns the
// pipe server's result (0 on a clean stop).
DWORD runServer(ServiceContext& ctx, void (*onRunning)()) {
    std::thread refresher([&] { runChargeRefresher(ctx); });
    std::thread monitor([&] { ctx.monitor.run(ctx); });
    DWORD pipeResult = 0;
    std::thread pipeThread([&] { pipeResult = runPipeServer(ctx); });
    if (onRunning) onRunning();

    pipeThread.join();  // returns on stop, or on a fatal pipe error
    SetEvent(ctx.stopEvent);
    monitor.join();
    refresher.join();
    return pipeResult;
}

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHintMs;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT) : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

const char* powerEventName(DWORD type) {
    switch (type) {
        case PBT_APMSUSPEND: return "suspend";
        case PBT_APMRESUMEAUTOMATIC: return "resume (automatic)";
        case PBT_APMRESUMESUSPEND: return "resume (user)";
        case PBT_APMPOWERSTATUSCHANGE: return "power status change";
        case PBT_POWERSETTINGCHANGE: return "power setting change";
        default: return "other";
    }
}

DWORD WINAPI controlHandler(DWORD control, DWORD eventType, LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            logLine(control == SERVICE_CONTROL_STOP ? "service: stop requested" : "service: system shutdown");
            reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
            if (g_ctx) SetEvent(g_ctx->stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_POWEREVENT:
            logf("service: power event: {} (0x{:X})", powerEventName(eventType), eventType);
            if (g_ctx && (eventType == PBT_APMRESUMEAUTOMATIC || eventType == PBT_APMRESUMESUSPEND)) {
                SetEvent(g_ctx->refreshEvent);  // BIOS settings may have changed meanwhile
                SetEvent(g_ctx->resumeEvent);   // check (and maybe re-apply) the thermal setting
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(ipc::kServiceName, controlHandler, nullptr);
    if (!g_statusHandle) return;
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    DWORD pipeResult = 0;
    {
        ServiceContext ctx;
        if (!ctx.ok()) {
            reportStatus(SERVICE_STOPPED, GetLastError());
            return;
        }
        g_ctx = &ctx;

        const auto m = machineInfo();
        logf("service: starting MyAlien {} on {} {} (BIOS {})", MYALIEN_VERSION, toUtf8(m.manufacturer),
             toUtf8(m.product), toUtf8(m.biosVersion));

        pipeResult = runServer(ctx, [] { reportStatus(SERVICE_RUNNING); });
        if (pipeResult != 0) logf("service: pipe server ended with error {}", pipeResult);
        logLine("service: stopped");
        g_ctx = nullptr;
    }
    reportStatus(SERVICE_STOPPED, pipeResult);
}

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_ctx) SetEvent(g_ctx->stopEvent);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Installation

struct ScCloser {
    void operator()(SC_HANDLE h) const noexcept {
        if (h) CloseServiceHandle(h);
    }
};
using UniqueSc = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScCloser>;

void fail(const char* what) {
    const DWORD err = GetLastError();
    std::fprintf(stderr, "error: %s: %s (%lu)\n", what, hrMessage(HRESULT_FROM_WIN32(err)).c_str(), err);
}

std::wstring programFilesDir() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path))) return {};
    std::wstring r(path);
    CoTaskMemFree(path);
    return r + L"\\MyAlien";
}

std::wstring selfPath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return (n == 0 || n == MAX_PATH) ? std::wstring() : std::wstring(path, n);
}

bool stopAndWait(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc, &st)) return false;
    if (st.dwCurrentState == SERVICE_STOPPED) return true;
    if (st.dwCurrentState != SERVICE_STOP_PENDING && !ControlService(svc, SERVICE_CONTROL_STOP, &st)) return false;
    for (int i = 0; i < 100; ++i) {  // up to 10 s
        Sleep(100);
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == SERVICE_STOPPED) return true;
    }
    return false;
}

// The executable may stay locked for a moment after the service process exits.
bool retry(auto&& op) {
    for (int i = 0; i < 20; ++i) {
        if (op()) return true;
        Sleep(250);
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
int runAsService() {
    initLog(LogTarget::File);
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(ipc::kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::fputs("myalien-svc is a Windows service. Use --install, --uninstall or --console (see --help).\n",
                       stderr);
            return 2;
        }
        return static_cast<int>(GetLastError());
    }
    return 0;
}

int runInConsole() {
    initLog(LogTarget::Console);
    ServiceContext ctx;
    if (!ctx.ok()) return 1;
    g_ctx = &ctx;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    logf("console: MyAlien {} pipe server, elevated: {}. Ctrl+C to stop.", MYALIEN_VERSION,
         isElevated() ? "yes" : "no");
    const DWORD r = runServer(ctx, nullptr);
    g_ctx = nullptr;
    return r == 0 ? 0 : 1;
}

int install() {
    if (!isElevated()) {
        std::fputs("Installing the service requires an elevated prompt (Run as administrator).\n", stderr);
        return 1;
    }
    const std::wstring dir = programFilesDir();
    const std::wstring src = selfPath();
    if (dir.empty() || src.empty()) {
        std::fputs("error: cannot resolve paths\n", stderr);
        return 1;
    }
    const std::wstring dst = dir + L"\\" + kExeName;

    UniqueSc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) return fail("OpenSCManager"), 1;

    // Existing service: stop it so the executable can be replaced.
    UniqueSc svc(OpenServiceW(scm.get(), ipc::kServiceName, SERVICE_ALL_ACCESS));
    if (svc) {
        std::puts("Stopping the existing service...");
        if (!stopAndWait(svc.get())) return fail("stopping the service"), 1;
    }

    // Program Files is writable by administrators only: a SYSTEM service must not run from a
    // user-writable directory such as the build tree.
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return fail("creating the install directory"), 1;
    if (_wcsicmp(src.c_str(), dst.c_str()) != 0 && !retry([&] { return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0; }))
        return fail("copying myalien-svc.exe"), 1;
    std::printf("Installed : %s\n", toUtf8(dst).c_str());

    const std::wstring binPath = L"\"" + dst + L"\"";
    if (svc) {
        if (!ChangeServiceConfigW(svc.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName))
            return fail("updating the service"), 1;
    } else {
        svc.reset(CreateServiceW(scm.get(), ipc::kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath.c_str(),
                                 nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr));
        if (!svc) return fail("creating the service"), 1;
    }

    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_DESCRIPTION, &desc);

    // Restart after 5 s on the first two failures, then give up until the reset period (1 day).
    SC_ACTION actions[] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 24 * 60 * 60;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    if (!StartServiceW(svc.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        return fail("starting the service"), 1;
    std::printf("Service   : %s registered and started (LocalSystem, automatic start)\n",
                toUtf8(ipc::kServiceName).c_str());
    std::printf("Log       : %s\\logs\\service.log\n", toUtf8(dir).c_str());
    return 0;
}

int uninstall() {
    if (!isElevated()) {
        std::fputs("Removing the service requires an elevated prompt (Run as administrator).\n", stderr);
        return 1;
    }
    UniqueSc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) return fail("OpenSCManager"), 1;
    UniqueSc svc(OpenServiceW(scm.get(), ipc::kServiceName, SERVICE_ALL_ACCESS));
    if (!svc) {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) return fail("opening the service"), 1;
        std::puts("Service   : not installed");
    } else {
        if (!stopAndWait(svc.get())) return fail("stopping the service"), 1;
        if (!DeleteService(svc.get())) return fail("deleting the service"), 1;
        std::puts("Service   : stopped and removed");
    }

    const std::wstring dir = programFilesDir();
    if (!dir.empty()) {
        const std::wstring exe = dir + L"\\" + kExeName;
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !retry([&] { return DeleteFileW(exe.c_str()) != 0; }))
            return fail("deleting the installed executable"), 1;
        std::printf("Removed   : %s (logs kept in %s\\logs)\n", toUtf8(exe).c_str(), toUtf8(dir).c_str());
    }
    return 0;
}

}  // namespace myalien::svc
