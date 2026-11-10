#pragma once

#include <windows.h>

namespace myalien::svc {

// Serves \\.\pipe\MyAlien until stopEvent is signalled. Blocking; run it on its own thread.
// Returns a Win32 error code (0 on a clean stop).
DWORD runPipeServer(HANDLE stopEvent);

}  // namespace myalien::svc
