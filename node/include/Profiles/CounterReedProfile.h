#pragma once
#include <RadioSecurityFrames.h>

#include <CommandSessionFrames.h>
#include <ProfileIds.h>
#include <TelemetryFrames.h>
#include <stddef.h>
#include <stdint.h>

#include "ArduinoEepromStorage.h"
#include "ConfirmedInput.h"
#include "CounterStorage.h"
#include "DebugLog.h"
#include "PolledReedInput.h"
#include "TelemetrySchedule.h"

namespace radiosensors {
namespace node {

class CounterReedProfile {
public:
    static constexpr uint16_t kProfileId =
        protocol::profileIdValue(protocol::ProfileId::PulseCounter);
    static constexpr size_t kTelemetrySize = protocol::kCounterTelemetrySize;

    // Starting from HIGH is correct for counting: a boot inside a LOW phase
    // first confirms a fall, so that pulse is counted once, on its rise.
    CounterReedProfile(const pin_size_t pin, const uint32_t minimumPhaseMs)
        : input_(pin),
          contact_(true),
          phase_(minimumPhaseMs, true),
          store_(eeprom_),
          results_(eeprom_),
          journal_(store_, results_) {}

    void begin() {
        input_.sleep();
        store_.load(count_);
        journal_.recover(count_);
#if defined(NODE_DEBUG)
        debugValue(F("cnt load="), count_);
#endif
    }

    void poll(const uint32_t now) {
        const InputChange contactChange = contact_.update(input_);
#if defined(NODE_DEBUG)
        if (contactChange != InputChange::None) {
            debugValue(contactChange == InputChange::Rose
                ? F("reed open @")
                : F("reed shut @"), now);
        }
#else
        (void)contactChange;
#endif
        if (phase_.update(now, contact_.high()) != InputChange::Rose) return;
        ++count_;
        store_.save(count_);
        reportSchedule_.pulseRecorded();
#if defined(NODE_DEBUG)
        debugValue(F("pulse "), count_);
#endif
    }

    bool reportDue(const uint32_t now) const {
        return reportSchedule_.due(now);
    }

    bool takeUrgentReport() { return reportSchedule_.takeUrgent(); }

    size_t encodeTelemetry(
        const protocol::TelemetryPrefix& prefix, uint8_t* output,
        const size_t capacity) {
        return protocol::encodeCounterTelemetry(
                   prefix, count_, output, capacity) ==
                protocol::TelemetryCodecStatus::Ok
            ? kTelemetrySize
            : 0;
    }

    void reportAcknowledged(const uint32_t now, uint16_t) {
        reportSchedule_.transmissionSucceeded(now);
    }

    void reportFailed(uint32_t, uint16_t) {}

    void applyCommand(
    const security::frames::Command& command, security::frames::CommandResult& result) {
        if (command.type != static_cast<uint8_t>(protocol::CommandType::SetCount)) {
            return;
        }
        if (!protocol::validCommandArguments(
                command.type, command.arguments, command.argumentSize)) {
            result.status = protocol::CommandStatus::InvalidArgument;
            return;
        }
        storage::SetCountResult stored{};
        if (journal_.apply(
                command.commandId, protocol::readUint32Le(command.arguments),
                count_, stored) == storage::SetCountOutcome::StorageFailure) {
            result.status = protocol::CommandStatus::StorageFailure;
            return;
        }
        result.status = protocol::CommandStatus::Applied;
        result.dataSize = protocol::kSetCountResultSize;
        protocol::writeUint32Le(result.data, stored.oldCount);
        protocol::writeUint32Le(result.data + 4, stored.appliedCount);
        reportSchedule_.countSet();
    }

    void commissioned() { journal_.forgetCommandIds(count_); }

private:
    PolledReedInput input_;
    ConfirmedInput contact_;
    MinimumPhaseFilter phase_;
    storage::ArduinoEepromStorage eeprom_;
    storage::CounterStore<storage::ArduinoEepromStorage> store_;
    storage::SetCountStore<storage::ArduinoEepromStorage> results_;
    storage::SetCountJournal<storage::ArduinoEepromStorage> journal_;
    CounterReportSchedule reportSchedule_;
    uint32_t count_ = 0;
};

}  // namespace node
}  // namespace radiosensors
