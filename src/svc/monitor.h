#pragma once

// Thermal monitor: the service's view of "what the firmware does" vs. "what the user chose".
//
//  * Watches the thermal state at start-up, after resume and every kPollMs.
//  * Remembers the last setting made through MyAlien (state.ini).
//  * Standalone (MyDell's FusionService not running): after start-up / resume, re-applies that
//    setting if the firmware no longer reports it. With MyDell running it only logs: MyDell owns
//    persistence then (PLAN.md §5).
//  * Conflict guard: a change made by anything else (MyDell, firmware, another tool) is logged
//    and marks the MyAlien setting "overridden": no re-apply until the next MyAlien change.
//    Re-apply happens at most once per start-up / resume, so there is never a write loop.

#include "svc/thermal_state.h"

#include <windows.h>

#include <mutex>
#include <optional>
#include <string>

namespace myalien::svc {

struct ServiceContext;

class Monitor {
public:
    static constexpr DWORD kPollMs = 15000;
    static constexpr DWORD kResumeSettleMs = 5000;  // let the firmware finish its own resume work

    // Thread body; returns when ctx.stopEvent is signalled.
    void run(ServiceContext& ctx);

    // Called by the pipe thread after a successful thermal write made through MyAlien.
    void recordWrite(const ThermalState& now);

private:
    void observe(Awcc& awcc, const char* reason, bool mayReapply);

    std::mutex mutex_;
    std::wstring statePath_;
    DesiredState desired_;
    std::optional<ThermalState> lastSeen_;
};

}  // namespace myalien::svc
