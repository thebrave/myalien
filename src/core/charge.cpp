#include "core/charge.h"

#include "core/bios_attributes.h"
#include "core/error.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace myalien {

ChargeSettings readChargeSettings(const BiosAttributes& bios) {
    const auto mode = bios.enumeration(attr::PrimaryBattChargeCfg);
    const auto start = bios.integer(attr::CustomChargeStart);
    const auto stop = bios.integer(attr::CustomChargeStop);
    if (!mode || !start || !stop) throw std::runtime_error("this BIOS does not expose the battery charge settings");

    ChargeSettings s;
    s.mode = mode->current;
    s.modes = mode->possible;
    s.start = start->current;
    s.startMin = start->lower;
    s.startMax = start->upper;
    s.stop = stop->current;
    s.stopMin = stop->lower;
    s.stopMax = stop->upper;
    return s;
}

std::vector<AttributeWrite> planChargeChange(const ChargeSettings& cur, const std::wstring& mode,
                                             const std::optional<ChargeRange>& range) {
    if (std::find(cur.modes.begin(), cur.modes.end(), mode) == cur.modes.end())
        throw std::invalid_argument(std::format("charge mode {} is not offered by this BIOS", toUtf8(mode)));

    if (range) {
        if (mode != kCustomMode) throw std::invalid_argument("a start/stop range can only be given with the Custom mode");
        if (range->start < cur.startMin || range->start > cur.startMax)
            throw std::invalid_argument(
                std::format("start {} % is outside {}..{} %", range->start, cur.startMin, cur.startMax));
        if (range->stop < cur.stopMin || range->stop > cur.stopMax)
            throw std::invalid_argument(
                std::format("stop {} % is outside {}..{} %", range->stop, cur.stopMin, cur.stopMax));
        if (range->stop - range->start < kMinChargeWindow)
            throw std::invalid_argument(
                std::format("stop must be at least {} % above start ({} - {})", kMinChargeWindow, range->start,
                            range->stop));
    }

    std::vector<AttributeWrite> writes;

    // The mode goes first: the thresholds are read-only unless the mode is Custom.
    if (cur.mode != mode) writes.push_back({attr::PrimaryBattChargeCfg, mode});

    if (range) {
        const AttributeWrite startW{attr::CustomChargeStart, std::to_wstring(range->start)};
        const AttributeWrite stopW{attr::CustomChargeStop, std::to_wstring(range->stop)};
        const bool startChanges = range->start != cur.start;
        const bool stopChanges = range->stop != cur.stop;

        // Keep stop - start >= 5 after each write:
        //  * stop moving up (or staying): raise stop first, then start;
        //  * stop moving down: lower start first, then stop.
        if (range->stop >= cur.stop) {
            if (stopChanges) writes.push_back(stopW);
            if (startChanges) writes.push_back(startW);
        } else {
            if (startChanges) writes.push_back(startW);
            if (stopChanges) writes.push_back(stopW);
        }
    }
    return writes;
}

}  // namespace myalien
