#pragma once

namespace myalien::svc {

// Entry point when started by the Service Control Manager. Returns the process exit code.
int runAsService();

// Foreground mode for development: same pipe server, Ctrl+C to stop, logs to stderr.
int runInConsole();

// Copies this executable to %ProgramFiles%\MyAlien, registers (or updates) the "MyAlien"
// service (LocalSystem, automatic start, restart on failure) and starts it. Elevation required.
int install();

// Stops and deletes the service, removes the installed executable. Elevation required.
int uninstall();

}  // namespace myalien::svc
