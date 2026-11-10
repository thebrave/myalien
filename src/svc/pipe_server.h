#pragma once

#include "svc/charge_cache.h"

#include <windows.h>

namespace myalien::svc {

// Serves \\.\pipe\MyAlien until ctx.stopEvent is signalled. Blocking; run it on its own thread.
// Returns a Win32 error code (0 on a clean stop).
DWORD runPipeServer(ServiceContext& ctx);

}  // namespace myalien::svc
