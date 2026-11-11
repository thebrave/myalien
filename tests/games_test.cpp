// Tests for the launcher file readers (core/text_formats) and the game matcher (core/games).

#include "core/games.h"
#include "core/text_formats.h"

#include <cstdio>

using namespace myalien;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

games::Game game(games::Source s, const wchar_t* name, const wchar_t* dir, const wchar_t* exe = L"") {
    games::Game g;
    g.source = s;
    g.name = name;
    g.dir = dir;
    g.exe = exe;
    return g;
}

}  // namespace

int main() {
    // --- KeyValues (Steam) ---
    {
        const auto root = text::parseKeyValues(
            "\xEF\xBB\xBF\"libraryfolders\"\n{\n"
            "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
            "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"420734912\"\n\t\t}\n\t}\n"
            "\t// comment\n"
            "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n");
        const auto* lf = root.find("LibraryFolders");
        expect(lf && lf->block && lf->children.size() == 2, "vdf: two libraries, case-insensitive key");
        expect(lf && lf->children[0].get("path") == "C:\\Program Files (x86)\\Steam", "vdf: escaped backslashes");
        expect(lf && lf->children[1].get("path") == "D:\\SteamLibrary", "vdf: after comment");
        expect(lf && lf->children[0].find("apps") && lf->children[0].find("apps")->block, "vdf: nested block");
        expect(lf && lf->children[0].get("apps").empty(), "vdf: get() on a block is empty");
    }
    {
        const auto root = text::parseKeyValues("\"LibraryFolders\" { \"TimeNextStatsReport\" \"1\" \"1\" \"E:\\\\Games\" }");
        const auto* lf = root.find("libraryfolders");
        expect(lf && lf->children.size() == 2 && lf->children[1].value == "E:\\Games", "vdf: old flat format");
    }
    {
        const auto acf = text::parseKeyValues(
            "\"AppState\"\n{\n\t\"appid\"\t\t\"1091500\"\n\t\"name\"\t\t\"Say \\\"Hi\\\"\"\n"
            "\t\"installdir\"\t\t\"Cyberpunk 2077\"\n}\n");
        const auto* st = acf.find("AppState");
        expect(st && st->get("installdir") == "Cyberpunk 2077" && st->get("appid") == "1091500", "acf fields");
        expect(st && st->get("name") == "Say \"Hi\"", "acf escaped quote");
    }
    {
        const auto broken = text::parseKeyValues("\"AppState\" { \"name\" ");
        expect(broken.find("AppState") != nullptr, "vdf: truncated input does not crash");
        expect(text::parseKeyValues("").children.empty(), "vdf: empty");
    }

    // --- JSON (Epic) ---
    {
        const auto m = text::parseJsonObject(
            "{\n  \"FormatVersion\": 0,\n  \"bIsIncompleteInstall\": false,\n"
            "  \"DisplayName\": \"Caf\\u00e9 \\\"Racer\\\" \\ud83c\\udfae\",\n"
            "  \"InstallLocation\": \"C:\\\\Program Files\\\\Epic Games\\\\Racer\",\n"
            "  \"AppCategories\": [ \"public\", \"games\", \"applications\" ],\n"
            "  \"Nested\": { \"a\": \"}\", \"b\": [1, 2] },\n"
            "  \"AppName\": \"Racer\"\n}");
        expect(m.size() == 7, "json: all top-level members");
        expect(m.count("DisplayName") && m.at("DisplayName").isString &&
                   m.at("DisplayName").text == "Caf\xC3\xA9 \"Racer\" \xF0\x9F\x8E\xAE",
               "json: \\u escapes and surrogate pair");
        expect(m.count("InstallLocation") && m.at("InstallLocation").text == "C:\\Program Files\\Epic Games\\Racer",
               "json: backslashes");
        expect(m.count("bIsIncompleteInstall") && !m.at("bIsIncompleteInstall").isString &&
                   m.at("bIsIncompleteInstall").text == "false",
               "json: bool kept raw");
        expect(m.count("AppCategories") && m.at("AppCategories").text.find("\"games\"") != std::string::npos,
               "json: array kept raw");
        expect(m.count("AppName") && m.at("AppName").text == "Racer", "json: member after nested object with '}'");
    }
    {
        expect(text::parseJsonObject("[1,2]").empty(), "json: not an object");
        expect(text::parseJsonObject("{\"a\": \"unterminated").empty(), "json: truncated string");
        expect(text::parseJsonObject("{}").empty(), "json: empty object");
    }

    // --- YAML (Riot) ---
    {
        const std::string y =
            "auto_patching_enabled_by_player: false\n"
            "product_install_full_path: \"C:/Riot Games/VALORANT/live\"\r\n"
            "product_install_root: \"C:/Riot Games\"\n";
        expect(text::yamlScalar(y, "product_install_full_path") == "C:/Riot Games/VALORANT/live", "yaml: quoted, CRLF");
        expect(text::yamlScalar(y, "product_install_root") == "C:/Riot Games", "yaml: other key");
        expect(!text::yamlScalar(y, "product_install").has_value(), "yaml: no prefix match");
    }

    // --- Matcher ---
    {
        using games::Source;
        const games::Matcher m({
            game(Source::Steam, L"Cyberpunk 2077", L"D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077"),
            game(Source::Epic, L"Foo", L"C:\\Games\\Foo"),
            game(Source::Epic, L"Foo Tools", L"C:\\Games\\Foo\\Tools"),
            game(Source::Manual, L"Root", L"E:"),
            game(Source::GameBar, L"Loose", L"C:\\Tools", L"C:\\Tools\\loose.exe"),
        });
        expect(m.games().size() == 4, "matcher: drive root rejected");
        const auto* g = m.match(L"D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe");
        expect(g && g->name == L"Cyberpunk 2077", "matcher: exe in a subfolder");
        g = m.match(L"d:\\steamlibrary\\STEAMAPPS\\common\\cyberpunk 2077\\REDprelauncher.exe");
        expect(g && g->name == L"Cyberpunk 2077", "matcher: case-insensitive");
        expect(!m.match(L"C:\\Games\\FooBar\\foobar.exe"), "matcher: no partial folder-name match");
        expect(!m.match(L"C:\\Games\\Foo"), "matcher: the folder itself is not an image under it");
        g = m.match(L"C:\\Games\\Foo\\Tools\\editor.exe");
        expect(g && g->name == L"Foo Tools", "matcher: deepest folder wins");
        g = m.match(L"C:\\Tools\\loose.exe");
        expect(g && g->name == L"Loose", "matcher: Game Bar exact exe");
        expect(!m.match(L"C:\\Tools\\other.exe"), "matcher: Game Bar entry does not cover its folder");
        expect(!m.match(L"C:\\Games\\Foo\\UnityCrashHandler64.exe"), "matcher: crash handler excluded");
        expect(!m.match(L"C:\\Games\\Foo\\steam.exe"), "matcher: launcher exe excluded");
        expect(!m.match(L""), "matcher: empty image");
    }
    {
        expect(games::normalizePath(L"C:/Riot Games/VALORANT/live/") == L"C:\\Riot Games\\VALORANT\\live",
               "normalizePath: slashes and trailing separator");
        expect(games::normalizePath(L"C:\\Games\\..\\Games\\X") == L"C:\\Games\\X", "normalizePath: dot-dot");
        expect(games::normalizePath(L"") .empty(), "normalizePath: empty");
    }

    // --- User choices (exclusions, manual folders) ---
    {
        using games::Source;
        const std::vector<games::Game> scanned{
            game(Source::Steam, L"Zeta", L"D:\\Steam\\common\\Zeta"),
            game(Source::GameBar, L"Edge", L"C:\\Edge", L"C:\\Edge\\msedge.exe"),
            game(Source::Epic, L"Alpha", L"C:\\Games\\Alpha"),
        };
        const auto out = games::applyUserChoices(
            scanned, {L"C:\\Games\\Beta\\", L"c:\\games\\alpha", L"C:\\", L"C:\\Windows"},
            {L"c:\\edge\\MSEDGE.EXE", L"D:\\Other"});
        expect(out.size() == 3, "choices: one excluded, one manual added, duplicates and broad folders skipped");
        expect(out.size() == 3 && out[0].name == L"Alpha" && out[1].name == L"Beta" && out[2].name == L"Zeta",
               "choices: sorted by name");
        expect(out.size() == 3 && out[1].source == Source::Manual && out[1].dir == L"C:\\Games\\Beta",
               "choices: manual folder normalized");
        expect(games::gameKey(scanned[1]) == L"C:\\Edge\\msedge.exe" && games::gameKey(scanned[0]) == L"D:\\Steam\\common\\Zeta",
               "gameKey: exe for Game Bar, folder otherwise");
        expect(games::isTooBroad(L"C:\\") && games::isTooBroad(L"C:\\Windows\\") && !games::isTooBroad(L"C:\\Games\\X"),
               "isTooBroad");
    }

    if (failures == 0) std::puts("games_test: all passed");
    return failures == 0 ? 0 : 1;
}
