#include "core/games.h"

#include "core/error.h"
#include "core/text_formats.h"

#include <windows.h>
#include <shlwapi.h>
#include <winsqlite/winsqlite3.h>
#include <wrl/client.h>
#include <xmllite.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <memory>
#include <type_traits>

namespace myalien::games {

namespace {

using Microsoft::WRL::ComPtr;

// --- small helpers ----------------------------------------------------------------------

bool ieq(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

bool istartsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && ieq(s.substr(0, prefix.size()), prefix);
}

bool icontains(std::wstring_view s, std::wstring_view part) {
    if (part.size() > s.size()) return false;
    for (size_t i = 0; i + part.size() <= s.size(); ++i)
        if (ieq(s.substr(i, part.size()), part)) return true;
    return false;
}

int icompare(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE);
}

std::wstring expandEnv(const wchar_t* s) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(s, buf, static_cast<DWORD>(std::size(buf)));
    return n == 0 || n > std::size(buf) ? std::wstring() : std::wstring(buf);
}

bool isDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring lastComponent(const std::wstring& path) {
    const size_t p = path.find_last_of(L'\\');
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring parentDir(const std::wstring& path) {
    const size_t p = path.find_last_of(L'\\');
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

// Whole file as bytes (UTF-8 text). Empty when missing or larger than `cap`.
std::string readFile(const std::wstring& path, DWORD cap = 4 * 1024 * 1024) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= cap) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) read = 0;
        data.resize(read);
    }
    CloseHandle(h);
    return data;
}

// Names of the entries matching `pattern` (e.g. L"C:\\x\\*.acf"); directories or files only.
std::vector<std::wstring> listDir(const std::wstring& pattern, bool dirs) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const bool isD = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isD != dirs) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        names.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

struct KeyCloser {
    void operator()(HKEY k) const noexcept { RegCloseKey(k); }
};
using UniqueKey = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

UniqueKey openKey(HKEY root, const std::wstring& path) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return nullptr;
    return UniqueKey(k);
}

std::vector<std::wstring> subkeys(HKEY key) {
    std::vector<std::wstring> names;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        const LSTATUS st = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st == ERROR_SUCCESS) names.emplace_back(name, len);
    }
    return names;
}

std::wstring regStr(HKEY key, const std::wstring& sub, const wchar_t* value) {
    DWORD bytes = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    const wchar_t* subp = sub.empty() ? nullptr : sub.c_str();
    if (RegGetValueW(key, subp, value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subp, value, flags, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

std::wstring regStr(HKEY root, const std::wstring& path, const std::wstring& sub, const wchar_t* value) {
    const auto k = openKey(root, path);
    return k ? regStr(k.get(), sub, value) : std::wstring();
}

std::wstring utf8(std::string_view s) { return toWide(s); }

// --- collection -------------------------------------------------------------------------

struct Collector {
    std::vector<Game> games;
    std::vector<SourceReport> reports;

    SourceReport& report(Source s) {
        for (auto& r : reports)
            if (r.source == s) return r;
        reports.push_back({s});
        return reports.back();
    }

    void add(Source s, std::wstring name, const std::wstring& dir, std::wstring exe = {}, std::wstring id = {}) {
        Game g;
        g.source = s;
        g.dir = normalizePath(dir);
        g.exe = exe.empty() ? std::wstring() : normalizePath(exe);
        g.name = name.empty() ? lastComponent(g.dir) : std::move(name);
        g.id = std::move(id);
        if (g.dir.empty()) return;
        games.push_back(std::move(g));
    }
};

struct UninstallEntry {
    std::wstring key, name, location, uninstall;
};

std::vector<UninstallEntry> uninstallEntries() {
    static constexpr wchar_t k64[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    static constexpr wchar_t k32[] = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    const std::pair<HKEY, const wchar_t*> roots[] = {
        {HKEY_LOCAL_MACHINE, k64}, {HKEY_LOCAL_MACHINE, k32}, {HKEY_CURRENT_USER, k64}};
    std::vector<UninstallEntry> out;
    for (const auto& [root, path] : roots) {
        const auto k = openKey(root, path);
        if (!k) continue;
        for (const auto& sub : subkeys(k.get())) {
            UninstallEntry e{sub, regStr(k.get(), sub, L"DisplayName"), regStr(k.get(), sub, L"InstallLocation"),
                             regStr(k.get(), sub, L"UninstallString")};
            if (!e.location.empty()) out.push_back(std::move(e));
        }
    }
    return out;
}

void scanSteam(Collector& c) {
    auto& r = c.report(Source::Steam);
    std::wstring steam = regStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"", L"InstallPath");
    if (steam.empty()) steam = regStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"", L"SteamPath");
    steam = normalizePath(steam);
    if (steam.empty() || !isDir(steam)) return;
    r.present = true;

    std::vector<std::wstring> libraries{steam};
    const auto vdf = text::parseKeyValues(readFile(steam + L"\\steamapps\\libraryfolders.vdf"));
    if (const auto* root = vdf.find("libraryfolders")) {
        for (const auto& entry : root->children) {
            // Current format: "0" { "path" "..." }. Old format: "1" "D:\\SteamLibrary".
            const std::string path = entry.block ? entry.get("path") : entry.value;
            if (path.empty() || entry.key.find_first_not_of("0123456789") != std::string::npos) continue;
            const std::wstring lib = normalizePath(utf8(path));
            if (std::none_of(libraries.begin(), libraries.end(), [&](const auto& l) { return ieq(l, lib); }))
                libraries.push_back(lib);
        }
    }
    for (const auto& lib : libraries) {
        const std::wstring apps = lib + L"\\steamapps\\";
        for (const auto& file : listDir(apps + L"appmanifest_*.acf", false)) {
            const auto acf = text::parseKeyValues(readFile(apps + file));
            const auto* state = acf.find("AppState");
            if (!state) continue;
            const std::string appid = state->get("appid"), name = state->get("name"), dir = state->get("installdir");
            if (dir.empty() || appid == "228980" || name.find("Redistributable") != std::string::npos) continue;
            c.add(Source::Steam, utf8(name), apps + L"common\\" + utf8(dir), {}, utf8(appid));
        }
    }
}

void scanEpic(Collector& c) {
    auto& r = c.report(Source::Epic);
    const std::wstring dir = expandEnv(L"%ProgramData%\\Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (dir.empty() || !isDir(dir)) return;
    r.present = true;
    for (const auto& file : listDir(dir + L"\\*.item", false)) {
        const auto m = text::parseJsonObject(readFile(dir + L"\\" + file));
        auto get = [&](const char* k) {
            const auto it = m.find(k);
            return it == m.end() ? std::string() : it->second.text;
        };
        if (get("bIsIncompleteInstall") == "true") continue;
        const std::string app = get("AppName"), main = get("MainGameAppName");
        if (!main.empty() && main != app) continue;  // DLC
        const std::string categories = get("AppCategories");
        if (!categories.empty() && categories.find("\"games\"") == std::string::npos) continue;  // engines, tools
        const std::wstring install = utf8(get("InstallLocation"));
        if (install.empty()) continue;
        std::wstring exe = utf8(get("LaunchExecutable"));
        if (!exe.empty()) exe = install + L"\\" + exe;
        c.add(Source::Epic, utf8(get("DisplayName")), install, exe, utf8(app));
    }
}

void scanUbisoft(Collector& c) {
    auto& r = c.report(Source::Ubisoft);
    const auto k = openKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs");
    if (!k) return;
    r.present = true;
    for (const auto& id : subkeys(k.get())) {
        const std::wstring dir = regStr(k.get(), id, L"InstallDir");
        if (dir.empty()) continue;
        const std::wstring name =
            regStr(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Uplay Install " + id, L"",
                   L"DisplayName");
        c.add(Source::Ubisoft, name, dir, {}, id);
    }
}

void scanBattleNet(Collector& c, const std::vector<UninstallEntry>& entries) {
    auto& r = c.report(Source::BattleNet);
    for (const auto& e : entries) {
        if (!icontains(e.uninstall, L"Battle.net") || !icontains(e.uninstall, L"--uid=")) continue;
        r.present = true;
        if (ieq(e.name, L"Battle.net") || icontains(e.uninstall, L"--uid=battle.net")) continue;
        if (e.name.ends_with(L"Test") || e.name.ends_with(L"Beta") || e.name.find(L"PTR") != std::wstring::npos)
            continue;
        c.add(Source::BattleNet, e.name, e.location);
    }
}

void scanGog(Collector& c) {
    auto& r = c.report(Source::Gog);
    const auto k = openKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games");
    if (!k) return;
    r.present = true;
    for (const auto& id : subkeys(k.get())) {
        if (!regStr(k.get(), id, L"dependsOn").empty()) continue;  // DLC
        c.add(Source::Gog, regStr(k.get(), id, L"gameName"), regStr(k.get(), id, L"path"),
              regStr(k.get(), id, L"exe"), id);
    }
}

void scanEa(Collector& c, const std::vector<UninstallEntry>& entries) {
    auto& r = c.report(Source::Ea);
    for (const wchar_t* path : {L"SOFTWARE\\EA Games", L"SOFTWARE\\WOW6432Node\\EA Games"}) {
        const auto k = openKey(HKEY_LOCAL_MACHINE, path);
        if (!k) continue;
        r.present = true;
        for (const auto& sub : subkeys(k.get())) {
            const std::wstring dir = regStr(k.get(), sub, L"Install Dir");
            if (dir.empty()) continue;
            std::wstring name = regStr(k.get(), sub, L"DisplayName");
            c.add(Source::Ea, name.empty() ? sub : name, dir);
        }
    }
    if (openKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Electronic Arts\\EA Desktop")) r.present = true;
    for (const auto& e : entries) {
        if (!icontains(e.uninstall, L"EAInstaller")) continue;
        r.present = true;
        c.add(Source::Ea, e.name, e.location);
    }
}

void scanRiot(Collector& c, const std::vector<UninstallEntry>& entries) {
    auto& r = c.report(Source::Riot);
    static constexpr std::pair<const wchar_t*, const wchar_t*> kNames[] = {
        {L"valorant", L"VALORANT"},
        {L"league_of_legends", L"League of Legends"},
        {L"bacon", L"Legends of Runeterra"},
        {L"lion", L"2XKO"},
    };
    auto title = [](const std::wstring& product) {
        for (const auto& [id, name] : kNames)
            if (ieq(product, id)) return std::wstring(name);
        return product;
    };
    const std::wstring meta = expandEnv(L"%ProgramData%\\Riot Games\\Metadata");
    if (!meta.empty() && isDir(meta)) {
        r.present = true;
        for (const auto& patch : listDir(meta + L"\\*.live", true)) {
            const auto yaml = readFile(meta + L"\\" + patch + L"\\" + patch + L".product_settings.yaml");
            const auto path = text::yamlScalar(yaml, "product_install_full_path");
            if (!path || path->empty()) continue;
            const std::wstring product = patch.substr(0, patch.size() - 5);  // strip ".live"
            c.add(Source::Riot, title(product), utf8(*path), {}, product);
        }
    }
    for (const auto& e : entries) {
        if (!istartsWith(e.key, L"Riot Game ") || !e.key.ends_with(L".live")) continue;
        r.present = true;
        c.add(Source::Riot, e.name, e.location);
    }
}

void scanRockstar(Collector& c) {
    auto& r = c.report(Source::Rockstar);
    const auto k = openKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games");
    if (!k) return;
    r.present = true;
    for (const auto& title : subkeys(k.get())) {
        if (icontains(title, L"Launcher") || icontains(title, L"Social Club")) continue;
        const std::wstring dir = regStr(k.get(), title, L"InstallFolder");
        if (!dir.empty()) c.add(Source::Rockstar, title, dir);
    }
}

// DefaultDisplayName from MicrosoftGame.config, or "" (missing, or an ms-resource reference).
std::wstring xboxDisplayName(const std::wstring& config) {
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(config.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                      nullptr, &stream)))
        return {};
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(stream.Get())))
        return {};
    XmlNodeType type;
    while (reader->Read(&type) == S_OK) {
        if (type != XmlNodeType_Element) continue;
        const wchar_t* local = nullptr;
        if (FAILED(reader->GetLocalName(&local, nullptr)) || wcscmp(local, L"ShellVisuals") != 0) continue;
        const wchar_t* value = nullptr;
        if (reader->MoveToAttributeByName(L"DefaultDisplayName", nullptr) != S_OK ||
            FAILED(reader->GetValue(&value, nullptr)) || !value)
            return {};
        const std::wstring name(value);
        return istartsWith(name, L"ms-resource") ? std::wstring() : name;
    }
    return {};
}

// Library folders listed in <drive>\.GamingRoot ("RGBX", count, UTF-16 NUL-terminated names).
std::vector<std::wstring> gamingRoots(const std::wstring& drive) {
    std::vector<std::wstring> roots;
    const std::string data = readFile(drive + L".GamingRoot", 4096);
    if (data.size() >= 8 && data.compare(0, 4, "RGBX") == 0) {
        std::wstring name;
        for (size_t i = 8; i + 1 < data.size(); i += 2) {
            const wchar_t ch = static_cast<wchar_t>(static_cast<unsigned char>(data[i]) |
                                                    (static_cast<unsigned char>(data[i + 1]) << 8));
            if (ch == L'\0') {
                if (!name.empty()) roots.push_back(drive + name);
                name.clear();
            } else {
                name += ch;
            }
        }
        if (!name.empty()) roots.push_back(drive + name);
    } else if (isDir(drive + L"XboxGames")) {
        roots.push_back(drive + L"XboxGames");
    }
    return roots;
}

void scanXbox(Collector& c) {
    auto& r = c.report(Source::Xbox);
    wchar_t drives[256];
    const DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    if (n == 0 || n > std::size(drives)) return;
    for (const wchar_t* d = drives; *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        for (const auto& root : gamingRoots(d)) {
            if (!isDir(root)) continue;
            r.present = true;
            for (const auto& child : listDir(root + L"\\*", true)) {
                const std::wstring dir = root + L"\\" + child;
                const std::wstring config = dir + L"\\Content\\MicrosoftGame.config";
                if (!isFile(config)) continue;
                c.add(Source::Xbox, xboxDisplayName(config), dir);
            }
        }
    }
}

// file: URI for SQLite (forward slashes, percent-encoded), opened immutable: the Amazon client
// may hold the database open, and we only ever read it.
std::string sqliteUri(const std::wstring& path) {
    std::string uri = "file:///";
    for (unsigned char ch : toUtf8(path)) {
        if (ch == '\\') ch = '/';
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' ||
            ch == ':' || ch == '-' || ch == '.' || ch == '_' || ch == '~')
            uri += static_cast<char>(ch);
        else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            uri += '%';
            uri += kHex[ch >> 4];
            uri += kHex[ch & 0xF];
        }
    }
    return uri + "?immutable=1";
}

void scanAmazon(Collector& c) {
    auto& r = c.report(Source::Amazon);
    const std::wstring db = expandEnv(L"%LOCALAPPDATA%\\Amazon Games\\Data\\Games\\Sql\\GameInstallInfo.sqlite");
    if (db.empty() || !isFile(db)) return;
    r.present = true;
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(sqliteUri(db).c_str(), &h, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        r.error = h ? sqlite3_errmsg(h) : "cannot open the database";
        sqlite3_close(h);
        return;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h, "SELECT Id, ProductTitle, InstallDirectory FROM DbSet WHERE Installed = 1", -1, &st,
                           nullptr) != SQLITE_OK) {
        r.error = sqlite3_errmsg(h);
    } else {
        auto col = [&](int i) {
            const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
            return t ? utf8(t) : std::wstring();
        };
        while (sqlite3_step(st) == SQLITE_ROW) c.add(Source::Amazon, col(1), col(2), {}, col(0));
    }
    sqlite3_finalize(st);
    sqlite3_close(h);
}

// FileDescription of an executable, or its file name without extension.
std::wstring exeTitle(const std::wstring& exe) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, exe.c_str(), &ignored);
    if (size > 0) {
        std::vector<char> buf(size);
        if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, exe.c_str(), 0, size, buf.data())) {
            struct Lang {
                WORD lang, codepage;
            };
            Lang* langs = nullptr;
            UINT bytes = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &bytes) &&
                bytes >= sizeof(Lang)) {
                wchar_t q[64];
                swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].codepage);
                wchar_t* desc = nullptr;
                UINT len = 0;
                if (VerQueryValueW(buf.data(), q, reinterpret_cast<void**>(&desc), &len) && len > 1) {
                    std::wstring d(desc, wcsnlen(desc, len));
                    while (!d.empty() && iswspace(d.back())) d.pop_back();
                    if (!d.empty()) return d;
                }
            }
        }
    }
    std::wstring name = lastComponent(exe);
    if (const size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.resize(dot);
    return name;
}

void scanGameBar(Collector& c) {
    auto& r = c.report(Source::GameBar);
    const auto k = openKey(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children");
    if (!k) return;
    r.present = true;
    for (const auto& sub : subkeys(k.get())) {
        const std::wstring exe = normalizePath(regStr(k.get(), sub, L"MatchedExeFullPath"));
        if (exe.empty()) continue;
        Game g;
        g.source = Source::GameBar;
        g.exe = exe;
        g.dir = parentDir(exe);
        g.id = sub;
        if (!g.dir.empty()) c.games.push_back(std::move(g));  // name filled in after dedup
    }
}

// Folders that are never one game's directory, even if a catalog says so.
bool isTooBroad(const std::wstring& dir) {
    if (dir.find(L'\\', 2) == std::wstring::npos) return true;  // drive root
    for (const wchar_t* v : {L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%SystemRoot%", L"%USERPROFILE%",
                             L"%ProgramData%", L"%LOCALAPPDATA%", L"%APPDATA%"}) {
        const std::wstring p = normalizePath(expandEnv(v));
        if (!p.empty() && ieq(p, dir)) return true;
    }
    return false;
}

bool under(const std::wstring& path, const std::wstring& dir) {
    return path.size() > dir.size() && path[dir.size()] == L'\\' && istartsWith(path, dir);
}

// Keeps the first entry per directory (launchers before Game Bar), drops Game Bar entries
// already covered by a launcher directory, missing directories and too-broad folders.
std::vector<Game> finish(std::vector<Game> all) {
    std::vector<Game> out;
    for (auto& g : all) {
        if (g.source != Source::GameBar && isTooBroad(g.dir)) continue;
        if (g.source == Source::GameBar) {
            const bool covered = std::any_of(out.begin(), out.end(), [&](const Game& o) {
                return o.source == Source::GameBar ? ieq(o.exe, g.exe) : under(g.exe, o.dir);
            });
            if (covered || !isFile(g.exe)) continue;
            g.name = exeTitle(g.exe);
        } else {
            const bool dup =
                std::any_of(out.begin(), out.end(), [&](const Game& o) { return o.source != Source::GameBar && ieq(o.dir, g.dir); });
            if (dup || !isDir(g.dir)) continue;
        }
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return icompare(a.name, b.name) == CSTR_LESS_THAN; });
    return out;
}

constexpr const wchar_t* kLauncherExes[] = {
    L"steam.exe",          L"steamwebhelper.exe",       L"EpicGamesLauncher.exe",  L"EpicWebHelper.exe",
    L"upc.exe",            L"UplayWebCore.exe",         L"Battle.net.exe",         L"GalaxyClient.exe",
    L"EADesktop.exe",      L"RiotClientServices.exe",   L"RiotClientUx.exe",       L"Amazon Games UI.exe",
    L"XboxPcApp.exe",      L"GameBar.exe",              L"CrashReportClient.exe",  L"UnityCrashHandler64.exe",
    L"UnityCrashHandler32.exe",
};

}  // namespace

const char* toString(Source s) {
    switch (s) {
        case Source::Steam: return "Steam";
        case Source::Epic: return "Epic Games";
        case Source::Ubisoft: return "Ubisoft Connect";
        case Source::BattleNet: return "Battle.net";
        case Source::Gog: return "GOG Galaxy";
        case Source::Ea: return "EA app";
        case Source::Riot: return "Riot Client";
        case Source::Rockstar: return "Rockstar";
        case Source::Xbox: return "Xbox app";
        case Source::Amazon: return "Amazon Games";
        case Source::GameBar: return "Game Bar";
        case Source::Manual: return "Manual";
    }
    return "?";
}

std::wstring normalizePath(std::wstring path) {
    while (!path.empty() && (path.back() == L'\0' || iswspace(path.back()))) path.pop_back();
    if (path.empty()) return path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf)) path.assign(buf, n);
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    return path;
}

Catalog scan() {
    const auto start = std::chrono::steady_clock::now();
    Collector c;
    const auto uninstall = uninstallEntries();
    scanSteam(c);
    scanEpic(c);
    scanUbisoft(c);
    scanBattleNet(c, uninstall);
    scanGog(c);
    scanEa(c, uninstall);
    scanRiot(c, uninstall);
    scanRockstar(c);
    scanXbox(c);
    scanAmazon(c);
    scanGameBar(c);

    Catalog cat;
    cat.games = finish(std::move(c.games));
    for (auto& r : c.reports) {
        r.games = static_cast<int>(
            std::count_if(cat.games.begin(), cat.games.end(), [&](const Game& g) { return g.source == r.source; }));
        cat.sources.push_back(std::move(r));
    }
    cat.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return cat;
}

Matcher::Matcher(std::vector<Game> games) {
    for (auto& g : games) {
        if (g.source == Source::GameBar ? g.exe.empty() : g.dir.find(L'\\', 2) == std::wstring::npos) continue;
        games_.push_back(std::move(g));
    }
}

const Game* Matcher::match(const std::wstring& imagePath) const {
    if (imagePath.empty()) return nullptr;
    const std::wstring file = lastComponent(imagePath);
    for (const wchar_t* l : kLauncherExes)
        if (ieq(file, l)) return nullptr;
    const Game* best = nullptr;
    for (const auto& g : games_) {
        if (g.source == Source::GameBar) {
            if (ieq(g.exe, imagePath)) return &g;
            continue;
        }
        // The deepest directory wins (a game installed inside another game's folder).
        if (under(imagePath, g.dir) && (!best || g.dir.size() > best->dir.size())) best = &g;
    }
    return best;
}

std::wstring foregroundImage() {
    const HWND w = GetForegroundWindow();
    if (!w) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == 0) return {};
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(buf));
    const bool ok = QueryFullProcessImageNameW(p, 0, buf, &len) != FALSE;
    CloseHandle(p);
    return ok ? std::wstring(buf, len) : std::wstring();
}

}  // namespace myalien::games
