#pragma once
#include <RadioSecurityFrames.h>

#include <CommandSessionFrames.h>
#include <ProfileIds.h>
#include <TelemetryFrames.h>
#include <stddef.h>
#include <stdint.h>

#include "ConfirmedInput.h"
#include "DebugLog.h"
#include "PolledReedInput.h"
#include "TelemetrySchedule.h"

namespace radiosensors {
namespace node {

// Reports state 1 while the contact is open. Whether that means an open door,
// an open window, or a raised float is installation metadata.
class BinaryInputProfile {
public:
    static constexpr uint16_t kProfileId =
        protocol::profileIdValue(protocol::ProfileId::Binary);
    static constexpr size_t kTelemetrySize = protocol::kBinaryTelemetrySize;

    explicit BinaryInputProfile(const pin_size_t pin)
        : input_(pin), contact_(true) {}

    void begin() {
        bool high = true;
        if (!input_.sample(high)) high = input_.readOnce();
        contact_ = ConfirmedInput(high);
#if defined(NODE_DEBUG)
        debugLine(high ? F("in open") : F("in shut"));
#endif
    }

    void poll(const uint32_t now) {
        const InputChange change = contact_.update(input_);
        if (change == InputChange::None) return;
        reportSchedule_.stateChanged();
#if defined(NODE_DEBUG)
        debugValue(change == InputChange::Rose
            ? F("in open @")
            : F("in shut @"), now);
#else
        (void)now;
#endif
    }

    bool reportDue(const uint32_t now) const {
        return reportSchedule_.due(now);
    }

    bool takeUrgentReport() { return reportSchedule_.takeUrgent(); }

    size_t encodeTelemetry(
        const protocol::TelemetryPrefix& prefix, uint8_t* output,
        const size_t capacity) {
        return protocol::encodeBinaryTelemetry(
                   prefix, contact_.high() ? 1 : 0, output, capacity) ==
                protocol::TelemetryCodecStatus::Ok
            ? kTelemetrySize
            : 0;
    }

    void reportAcknowledged(const uint32_t now, uint16_t) {
        reportSchedule_.transmissionSucceeded(now);
    }

    void reportFailed(uint32_t, uint16_t) {}

    void applyCommand(const security::frames::Command&, security::frames::CommandResult&) {}
    void commissioned() {}

private:
    PolledReedInput input_;
    ConfirmedInput contact_;
    BinaryReportSchedule reportSchedule_;
};

}  // namespace node
}  // namespace radiosensors
