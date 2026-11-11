#pragma once

// State shared by the service's threads: SCM / console front-end, pipe server, charge-settings
// refresher and thermal monitor.

#include "svc/charge_cache.h"
#include "svc/monitor.h"

#include <windows.h>

namespace myalien::svc {

struct ServiceContext {
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);      // manual reset
    HANDLE refreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // wake the charge refresher
    HANDLE resumeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // wake the monitor after resume
    ChargeCache charge;
    Monitor monitor;

    ServiceContext() = default;
    ServiceContext(const ServiceContext&) = delete;
    ServiceContext& operator=(const ServiceContext&) = delete;
    ~ServiceContext() {
        for (HANDLE h : {stopEvent, refreshEvent, resumeEvent})
            if (h) CloseHandle(h);
    }
    bool ok() const noexcept { return stopEvent && refreshEvent && resumeEvent; }
};

}  // namespace myalien::svc
