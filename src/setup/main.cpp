// myalien-setup.exe - installs, updates and removes MyAlien. Windows SDK only.
//
// The three programs travel inside this executable as resources. Install:
//   1. close running tray icons (they restore game mode first),
//   2. extract the payload to a temporary folder and run `myalien-svc.exe --install` from there
//      (it stops an existing service, copies itself to Program Files, registers and starts it),
//   3. copy the tray, the command line and this setup to %ProgramFiles%\MyAlien,
//   4. register the tray for every user at logon, a Start menu shortcut, and the Apps entry,
//   5. start the tray for the current user, not elevated.
// Uninstall reverses it, keeping logs and settings unless asked to remove them.
//
// Usage: myalien-setup [/install | /uninstall] [/quiet] [/nostart] [/purge]

// The shell automation headers need the full windows.h (the project defines WIN32_LEAN_AND_MEAN).
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kTitle[] = L"MyAlien Setup";
constexpr wchar_t kVersion[] = L"" MYALIEN_VERSION;
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MyAlien";
constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"MyAlien";
constexpr wchar_t kTrayClass[] = L"MyAlienTrayWindow";

// Payload resource IDs (res/setup.rc).
constexpr int kResService = 101, kResTray = 102, kResCli = 103;

struct Options {
    bool uninstall = false;
    bool quiet = false;
    bool start = true;
    bool purge = false;       // uninstall: also remove logs, service state and this user's settings
    DWORD waitPid = 0;        // uninstall from a temporary copy: wait for the original to exit
};

// --- small helpers --------------------------------------------------------------------------

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p))) return {};
    std::wstring r(p);
    CoTaskMemFree(p);
    return r;
}

std::wstring installDir() { return knownFolder(FOLDERID_ProgramFiles) + L"\\MyAlien"; }
std::wstring shortcutPath() { return knownFolder(FOLDERID_CommonPrograms) + L"\\MyAlien.lnk"; }

std::wstring selfPath() {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    return n == 0 || n == MAX_PATH ? std::wstring() : std::wstring(p, n);
}

std::wstring errorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + L" (" + std::to_wstring(err) + L")";
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Files can stay locked for a moment after their process exits.
template <typename F>
bool retry(F&& op) {
    for (int i = 0; i < 20; ++i) {
        if (op()) return true;
        Sleep(250);
    }
    return false;
}

// What setup did, line by line, for the final dialog.
struct Report {
    std::wstring text;
    bool failed = false;
    void ok(const std::wstring& s) { text += L"✓ " + s + L"\n"; }
    void warn(const std::wstring& s) { text += L"! " + s + L"\n"; }
    bool fail(const std::wstring& s) {
        text += L"✗ " + s + L"\n";
        failed = true;
        return false;
    }
};

bool writeResource(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data || size == 0) return false;
    return retry([&] {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(h, data, size, &written, nullptr) && written == size;
        CloseHandle(h);
        return ok;
    });
}

// Runs a program hidden and returns its exit code (-1 if it could not start); its console
// output is appended to `output`.
long run(const std::wstring& exe, const std::wstring& args, std::wstring& output) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    const bool started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi) != FALSE;
    CloseHandle(writeEnd);
    std::string bytes;
    if (started) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) && n > 0) bytes.append(buf, n);
        WaitForSingleObject(pi.hProcess, 60000);
    }
    CloseHandle(readEnd);
    if (!started) return -1;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    output += w;
    return static_cast<long>(code);
}

// --- registry -----------------------------------------------------------------------------

bool setString(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, key, name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool setDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(root, key, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

std::wstring getString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring installedVersion() { return getString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion"); }

// --- tray -----------------------------------------------------------------------------------

// Asks every tray icon on this desktop to quit (it restores game mode first), then waits.
void closeTrays(Report& rep) {
    std::vector<HANDLE> processes;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, kTrayClass, nullptr)) != nullptr;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) processes.push_back(p);
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
    for (HANDLE p : processes) {
        if (WaitForSingleObject(p, 20000) != WAIT_OBJECT_0) {
            TerminateProcess(p, 1);
            rep.warn(L"A tray icon did not close in time and was ended.");
        }
        CloseHandle(p);
    }
    if (!processes.empty()) rep.ok(L"Closed the running tray icon");
}

// Starts a program as the signed-in user, not elevated: asks the desktop's Explorer to run it.
bool startAsDesktopUser(const std::wstring& exe) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
    VARIANT empty{};
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp)
        return false;
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> app;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(disp.As(&provider)) || FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView)) ||
        FAILED(folderView->get_Application(&app)) || FAILED(app.As(&shell)))
        return false;
    BSTR file = SysAllocString(exe.c_str());
    VARIANT none{};
    const HRESULT hr = shell->ShellExecute(file, none, none, none, none);
    SysFreeString(file);
    return SUCCEEDED(hr);
}

// --- shortcut -------------------------------------------------------------------------------

bool createShortcut(const std::wstring& target, const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"Thermal and battery care for the Dell G15 5520");
    return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

// --- install / uninstall --------------------------------------------------------------------

bool install(const Options& o, Report& rep) {
    const std::wstring dir = installDir();
    if (dir.size() < 10) return rep.fail(L"Cannot find the Program Files folder.");

    closeTrays(rep);

    // Payload to a private temporary folder; the service installs itself from there.
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring stage = std::wstring(tmp) + L"MyAlienSetup-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(stage.c_str(), nullptr);
    const std::wstring svcTmp = stage + L"\\myalien-svc.exe";
    if (!writeResource(kResService, svcTmp)) return rep.fail(L"Cannot unpack the service: " + errorText(GetLastError()));

    std::wstring out;
    const long code = run(svcTmp, L"--install", out);
    DeleteFileW(svcTmp.c_str());
    RemoveDirectoryW(stage.c_str());
    if (code != 0) return rep.fail(L"Installing the service failed:\n" + out);
    rep.ok(L"Service installed and started");

    // Tray, command line, and this setup (for the Apps entry's Uninstall).
    const std::wstring tray = dir + L"\\myalien-tray.exe", cli = dir + L"\\myalien.exe", setup = dir + L"\\myalien-setup.exe";
    if (!writeResource(kResTray, tray)) return rep.fail(L"Cannot write " + tray + L": " + errorText(GetLastError()));
    if (!writeResource(kResCli, cli)) return rep.fail(L"Cannot write " + cli + L": " + errorText(GetLastError()));
    const std::wstring self = selfPath();
    if (_wcsicmp(self.c_str(), setup.c_str()) != 0 &&
        !retry([&] { return CopyFileW(self.c_str(), setup.c_str(), FALSE) != 0; }))
        return rep.fail(L"Cannot write " + setup + L": " + errorText(GetLastError()));
    rep.ok(L"Programs copied to " + dir);

    // Tray at logon for every user; each user can still turn it off from its menu.
    setString(HKEY_LOCAL_MACHINE, kRunKey, kValueName, L"\"" + tray + L"\"");
    // A per-user entry left by a development copy would start a second tray.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
    rep.ok(L"Tray icon starts at sign-in for every user");

    if (createShortcut(tray, shortcutPath())) rep.ok(L"Start menu shortcut");
    else rep.warn(L"Could not create the Start menu shortcut.");

    // Apps entry.
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", L"MyAlien");
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", kVersion);
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", L"MyAlien");
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayIcon", L"\"" + tray + L"\",0");
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", dir);
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"URLInfoAbout", L"https://github.com/thebrave/myalien");
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString", L"\"" + setup + L"\" /uninstall");
    setString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString", L"\"" + setup + L"\" /uninstall /quiet");
    setDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoModify", 1);
    setDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoRepair", 1);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    ULONGLONG bytes = 0;
    for (const auto& f : {tray, cli, setup, dir + L"\\myalien-svc.exe"})
        if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a)) bytes += a.nFileSizeLow;
    setDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"EstimatedSize", static_cast<DWORD>(bytes / 1024));
    rep.ok(L"Listed in Settings > Apps");

    if (o.start) {
        if (startAsDesktopUser(tray)) rep.ok(L"Tray icon started");
        else rep.warn(L"Start MyAlien from the Start menu (it could not be started from setup).");
    }
    return true;
}

bool uninstall(const Options& o, Report& rep) {
    const std::wstring dir = installDir();
    if (dir.size() < 10) return rep.fail(L"Cannot find the Program Files folder.");

    closeTrays(rep);  // while the service still runs: the tray restores game mode through it

    const std::wstring svc = dir + L"\\myalien-svc.exe";
    if (exists(svc)) {
        std::wstring out;
        if (run(svc, L"--uninstall", out) != 0) return rep.fail(L"Removing the service failed:\n" + out);
        rep.ok(L"Service stopped and removed");
    }

    for (const wchar_t* f : {L"\\myalien-tray.exe", L"\\myalien.exe", L"\\myalien-setup.exe"}) {
        const std::wstring p = dir + f;
        if (exists(p) && !retry([&] { return DeleteFileW(p.c_str()) != 0; }))
            rep.warn(L"Could not delete " + p + L": " + errorText(GetLastError()));
    }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, kValueName);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName);
    DeleteFileW(shortcutPath().c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    rep.ok(L"Programs, shortcut and Apps entry removed");

    if (o.purge) {
        // Logs and the service's remembered settings, and this user's tray settings.
        const std::wstring logs = dir + L"\\logs";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((logs + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do DeleteFileW((logs + L"\\" + fd.cFileName).c_str());
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(logs.c_str());
        DeleteFileW((dir + L"\\state.ini").c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\MyAlien");
        rep.ok(L"Logs and settings removed");
    }
    if (RemoveDirectoryW(dir.c_str())) rep.ok(L"Removed " + dir);
    else if (!o.purge) rep.ok(L"Logs and settings kept in " + dir);
    return true;
}

// --- UI ---------------------------------------------------------------------------------------

int choose(const std::wstring& title, const std::wstring& instruction, const std::wstring& content,
           const std::vector<std::pair<int, std::wstring>>& buttons, const wchar_t* checkbox, bool& checked,
           PCWSTR icon = nullptr) {
    std::vector<TASKDIALOG_BUTTON> b;
    for (const auto& [id, text] : buttons) b.push_back({id, text.c_str()});
    TASKDIALOGCONFIG c{sizeof(c)};
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | (checked ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    c.pszWindowTitle = title.c_str();
    c.hInstance = GetModuleHandleW(nullptr);
    c.pszMainIcon = icon ? icon : MAKEINTRESOURCEW(1);
    c.pszMainInstruction = instruction.c_str();
    c.pszContent = content.c_str();
    c.cButtons = static_cast<UINT>(b.size());
    c.pButtons = b.data();
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszVerificationText = checkbox;
    int pressed = IDCANCEL;
    BOOL verified = checked;
    TaskDialogIndirect(&c, &pressed, nullptr, &verified);
    checked = verified != FALSE;
    return pressed;
}

void showResult(const std::wstring& instruction, const Report& rep) {
    TASKDIALOGCONFIG c{sizeof(c)};
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = kTitle;
    c.hInstance = GetModuleHandleW(nullptr);
    c.pszMainIcon = rep.failed ? TD_ERROR_ICON : MAKEINTRESOURCEW(1);
    c.pszMainInstruction = instruction.c_str();
    c.pszContent = rep.text.c_str();
    c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}

Options parse() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (_wcsicmp(a.c_str(), L"/uninstall") == 0) o.uninstall = true;
        else if (_wcsicmp(a.c_str(), L"/quiet") == 0) o.quiet = true;
        else if (_wcsicmp(a.c_str(), L"/nostart") == 0) o.start = false;
        else if (_wcsicmp(a.c_str(), L"/purge") == 0) o.purge = true;
        else if (_wcsicmp(a.c_str(), L"/waitpid") == 0 && i + 1 < argc) o.waitPid = wcstoul(argv[++i], nullptr, 10);
    }
    LocalFree(argv);
    return o;
}

// Uninstall deletes the folder this program runs from: run from a temporary copy instead.
bool relaunchFromTemp(const Options& o) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring copy = std::wstring(tmp) + L"myalien-uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    if (!CopyFileW(selfPath().c_str(), copy.c_str(), FALSE)) return false;
    std::wstring cmd = L"\"" + copy + L"\" /uninstall /waitpid " + std::to_wstring(GetCurrentProcessId());
    if (o.quiet) cmd += L" /quiet";
    if (o.purge) cmd += L" /purge";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Options o = parse();

    if (o.waitPid) {  // temporary copy: let the original exit, and delete this copy at the next restart
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, o.waitPid)) {
            WaitForSingleObject(p, 30000);
            CloseHandle(p);
        }
        MoveFileExW(selfPath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    const std::wstring installed = installedVersion();

    // Asks (unless quiet or already asked), then removes; from a temporary copy when this program
    // is the installed one.
    auto removeFlow = [&](bool ask) {
        if (ask) {
            bool purge = false;
            const int r = choose(kTitle, L"Remove MyAlien " + (installed.empty() ? std::wstring(kVersion) : installed) + L"?",
                                 L"The service, the tray icon, the command line, the shortcut and the Apps entry are "
                                 L"removed. The thermal profile and charging mode stay as they are.",
                                 {{100, L"Remove MyAlien"}}, L"Also remove logs and settings", purge, TD_WARNING_ICON);
            if (r != 100) return 1;
            o.purge = purge;
        }
        const std::wstring dir = installDir();
        if (!o.waitPid && _wcsnicmp(selfPath().c_str(), dir.c_str(), dir.size()) == 0 && relaunchFromTemp(o)) return 0;
        Report rep;
        uninstall(o, rep);
        if (!o.quiet) showResult(rep.failed ? L"MyAlien could not be removed completely" : L"MyAlien was removed", rep);
        return rep.failed ? 1 : 0;
    };

    if (o.uninstall) return removeFlow(!o.quiet && !o.waitPid);

    if (!o.quiet) {
        std::wstring instruction, button;
        if (installed.empty()) {
            instruction = std::wstring(L"Install MyAlien ") + kVersion;
            button = L"Install";
        } else if (installed == kVersion) {
            instruction = std::wstring(L"Reinstall MyAlien ") + kVersion;
            button = L"Reinstall";
        } else {
            instruction = L"Update MyAlien " + installed + L" to " + kVersion;
            button = L"Update";
        }
        const std::wstring content =
            L"Installs to " + installDir() +
            L": the MyAlien service (runs as SYSTEM, talks to the Dell firmware), the tray icon (starts at sign-in "
            L"for every user) and the myalien command line.\n\nThermal and battery care for the Dell G15 5520. "
            L"Not made or endorsed by Dell. MIT License.";
        std::vector<std::pair<int, std::wstring>> buttons{{100, button}};
        if (!installed.empty()) buttons.push_back({101, L"Remove MyAlien"});
        bool start = true;
        const int r = choose(kTitle, instruction, content, buttons, L"Start MyAlien when done", start);
        if (r == 101) {
            o.uninstall = true;
            return removeFlow(true);
        }
        if (r != 100) return 1;
        o.start = start;
    }

    Report rep;
    install(o, rep);
    if (!o.quiet) showResult(rep.failed ? L"MyAlien could not be installed" : L"MyAlien is installed", rep);
    return rep.failed ? 1 : 0;
}
