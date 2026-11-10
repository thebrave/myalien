#include "svc/charge_cache.h"

#include "core/wmi.h"
#include "svc/log.h"

namespace myalien::svc {

void ChargeCache::refresh() {
    const ULONGLONG requested = GetTickCount64();
    std::lock_guard refreshLock(refreshMutex_);
    {
        // Another thread completed a read after we asked: its result is fresh enough.
        std::lock_guard lock(dataMutex_);
        if (valid_ && readTick_ >= requested) return;
    }

    SnapshotTiming t;
    const Snapshot s = collectSnapshot({.thermal = false, .battery = false, .charge = true}, &t);
    logf("charge settings read: {:.0f} ms ({} BIOS reads){}", t.biosMs, t.biosScans,
         s.chargeError ? " - error: " + s.chargeError.message : std::string());

    std::lock_guard lock(dataMutex_);
    data_ = s;
    valid_ = true;
    readTick_ = GetTickCount64();
}

void ChargeCache::apply(Snapshot& s) const {
    std::lock_guard lock(dataMutex_);
    if (!valid_) {
        Snapshot pending;
        pending.chargeError = {"charge settings not read yet (service starting)", false};
        copyChargeSettings(pending, s);
        return;
    }
    copyChargeSettings(data_, s);
    s.chargeAgeSeconds = static_cast<uint32_t>((GetTickCount64() - readTick_) / 1000);
}

void runChargeRefresher(ServiceContext& ctx) {
    wmi::ComApartment com;
    for (;;) {
        try {
            ctx.charge.refresh();
        } catch (const std::exception& e) {
            logf("charge settings refresh failed: {}", e.what());
        }
        const HANDLE handles[] = {ctx.stopEvent, ctx.refreshEvent};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, ChargeCache::kRefreshIntervalMs);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
    }
}

}  // namespace myalien::svc
