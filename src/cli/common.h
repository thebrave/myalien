#pragma once

// Helpers shared by the CLI commands.

#include "core/actions.h"
#include "core/snapshot.h"

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace myalien::cli {

// Battery section of `status` (health, charge, charge mode).
void printBatterySection(const Snapshot& s);

// Snapshot for display, plus a label of where it came from:
//  * elevated            -> collected locally;
//  * not elevated        -> asked to the MyAlien service when it is running (charge settings
//                           from its cache, or re-read first when freshCharge is set);
//  * otherwise           -> collected locally (Dell sections will show access denied).
// Requires COM initialised on the calling thread.
std::pair<Snapshot, std::string> obtainSnapshot(const SnapshotParts& parts, bool freshCharge = false);

// Performs a write: in-process when elevated (`local`), otherwise by sending `request` to the
// MyAlien service. Prints the action's output (and where it ran) and returns its exit code.
// Requires COM initialised on the calling thread.
int runWrite(std::string_view what, const std::string& request, const std::function<ActionOutput()>& local,
             DWORD timeoutMs = 15000);

// Prints an action's output lines to stdout / stderr.
void printOutput(const ActionOutput& o);

std::string toAscii(std::wstring_view s);  // command-line argument -> pipe token (non-ASCII -> '?')

}  // namespace myalien::cli
