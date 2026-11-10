#pragma once

// Service-side cache of the BIOS battery charge settings.
//
// Reading Dell BIOS attributes costs ~4-5 s on the G15 5520 (the provider reads every
// attribute from the BIOS), far too slow per request. These settings only change through a
// write (MyAlien or MyDell) or BIOS setup, so the service reads them:
//  * at start-up and after resume (refresher thread),
//  * every kRefreshInterval,
//  * on demand (STATUS FRESH), e.g. for `myalien battery`.

#include "core/snapshot.h"

#include <windows.h>

#include <mutex>

namespace myalien::svc {

class ChargeCache {
public:
    static constexpr DWORD kRefreshIntervalMs = 10 * 60 * 1000;

    // Reads the settings now (COM must be initialised on the calling thread). Concurrent callers
    // are serialised; a caller that waited for another refresh does not read again.
    void refresh();

    // Copies the cached settings (with their age) into s. Before the first refresh completes,
    // s gets a "not read yet" error for the charge section.
    void apply(Snapshot& s) const;

private:
    std::mutex refreshMutex_;     // one BIOS read at a time
    mutable std::mutex dataMutex_;
    Snapshot data_;               // only the charge fields are used
    bool valid_ = false;
    ULONGLONG readTick_ = 0;
};

// Shared between the SCM/console front-end, the refresher thread and the pipe server.
struct ServiceContext {
    HANDLE stopEvent = nullptr;
    HANDLE refreshEvent = nullptr;  // auto-reset: wake the refresher (resume, ...)
    ChargeCache charge;
};

// Refresher thread body: refreshes at start, then on refreshEvent or every interval, until stop.
void runChargeRefresher(ServiceContext& ctx);

}  // namespace myalien::svc
