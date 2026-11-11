// Tests for the pure parts of core/actions: output encoding and profile names.

#include "core/actions.h"
#include "core/awcc.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace myalien;

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    {
        ActionOutput a;
        a.out("Before  : Quiet (0xA3)");
        a.err("multi\nline");
        a.exitCode = exitcode::NotApplied;
        const ActionOutput b = decodeOutput(encodeOutput(a));
        expect(b.exitCode == 3, "exit code round-trips");
        expect(b.lines.size() == 2 && !b.lines[0].error && b.lines[1].error, "line kinds round-trip");
        expect(b.lines[0].text == "Before  : Quiet (0xA3)", "stdout text");
        expect(b.lines[1].text == "multi line", "newlines flattened");
    }
    {
        expect(throws([] { decodeOutput("o:no exit\n"); }), "missing exit rejected");
        expect(throws([] { decodeOutput("exit=x\n"); }), "bad exit rejected");
        expect(throws([] { decodeOutput("exit=0\nzzz\n"); }), "unknown line kind rejected");
    }
    {
        expect(parseProfileName("quiet") == profile::Quiet, "quiet");
        expect(parseProfileName("Balanced-Performance") == profile::BalancedPerformance, "case-insensitive");
        expect(parseProfileName("custom") == profile::Custom, "custom");
        expect(parseProfileName("0xa3") == profile::Quiet && parseProfileName("0XA3") == profile::Quiet, "hex");
        expect(!parseProfileName("0xZZ") && !parseProfileName("0x1") && !parseProfileName("turbo"), "rejects junk");
        expect(describeProfile(profile::Quiet) == "Quiet (0xA3)  -> quiet", "describeProfile");
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
