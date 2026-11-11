#include "cli/common.h"

#include "core/error.h"
#include "core/ipc.h"
#include "core/system.h"

#include <cstdio>
#include <format>

namespace myalien::cli {

std::pair<Snapshot, std::string> obtainSnapshot(const SnapshotParts& parts, bool freshCharge) {
    if (isElevated()) return {collectSnapshot(parts), "local (elevated)"};

    // A fresh read of the BIOS charge settings takes several seconds in the service.
    const auto r = freshCharge ? ipc::call("STATUS FRESH", 20000) : ipc::call("STATUS");
    if (r.response && r.response->ok) {
        try {
            return {decodeSnapshot(r.response->body), "MyAlien service"};
        } catch (const std::exception& e) {
            return {collectSnapshot(parts), std::format("local (not elevated; bad service answer: {})", e.what())};
        }
    }
    const std::string why = r.response ? r.response->error : toString(r.error);
    return {collectSnapshot(parts), std::format("local (not elevated; {})", why)};
}

void printOutput(const ActionOutput& o) {
    for (const auto& l : o.lines) {
        std::fputs(l.text.c_str(), l.error ? stderr : stdout);
        std::fputc('\n', l.error ? stderr : stdout);
    }
}

int runWrite(std::string_view what, const std::string& request, const std::function<ActionOutput()>& local,
             DWORD timeoutMs) {
    // The service is preferred even when elevated: it records thermal changes as the setting to
    // keep, and would otherwise see an in-process change as an external one.
    const auto r = ipc::call(request, timeoutMs);
    if (!r.response) {
        if (isElevated()) {
            const ActionOutput o = local();
            printOutput(o);
            return o.exitCode;
        }
        std::fprintf(stderr, "%.*s needs the MyAlien service or an elevated prompt (%s).\n",
                     static_cast<int>(what.size()), what.data(), toString(r.error));
        return exitcode::Error;
    }
    if (!r.response->ok) {
        std::fprintf(stderr, "MyAlien service: %s\n", r.response->error.c_str());
        return exitcode::Error;
    }
    try {
        const ActionOutput o = decodeOutput(r.response->body);
        std::puts("Via     : MyAlien service");
        printOutput(o);
        return o.exitCode;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "MyAlien service: bad answer (%s)\n", e.what());
        return exitcode::Error;
    }
}

std::string toAscii(std::wstring_view s) {
    std::string r;
    for (wchar_t c : s) r += (c > L' ' && c < 0x7F) ? static_cast<char>(c) : '?';
    return r;
}

}  // namespace myalien::cli
