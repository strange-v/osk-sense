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
#include "Sht40Sensor.h"
#include "TelemetrySchedule.h"

namespace radiosensors {
namespace node {

class BinarySht40Profile {
public:
    static constexpr uint16_t kProfileId =
        protocol::profileIdValue(protocol::ProfileId::BinaryClimateTh);
    static constexpr size_t kTelemetrySize =
        protocol::kBinaryClimateThTelemetrySize;

    BinarySht40Profile(
        const pin_size_t pin, TwoWire& wire, const uint8_t address,
        const uint32_t reportIntervalMs)
        : input_(pin), contact_(true), climate_(wire, address),
          reportSchedule_(reportIntervalMs) {}

    void begin() {
        bool high = true;
        if (!input_.sample(high)) high = input_.readOnce();
        contact_ = ConfirmedInput(high);
        climate_.begin();
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
        int16_t temperature = protocol::kInvalidTemperature;
        uint16_t humidity = protocol::kInvalidHumidity;
        if (!climate_.read(temperature, humidity)) {
#if defined(NODE_DEBUG)
            debugLine(F("sht fail"));
#endif
        } else {
#if defined(NODE_DEBUG)
            int32_t positiveTemperature = temperature;
            Serial.print(F("sht t="));
            if (positiveTemperature < 0) {
                Serial.print('-');
                positiveTemperature = -positiveTemperature;
            }
            Serial.print(positiveTemperature / 100);
            Serial.print('.');
            if (positiveTemperature % 100 < 10) Serial.print('0');
            Serial.print(positiveTemperature % 100);
            Serial.print(F("C rh="));
            Serial.print(humidity / 100);
            Serial.print('.');
            if (humidity % 100 < 10) Serial.print('0');
            Serial.print(humidity % 100);
            Serial.println('%');
#endif
        }
        return protocol::encodeBinaryClimateThTelemetry(
                   prefix, contact_.high() ? 1 : 0, temperature, humidity,
                   output, capacity) == protocol::TelemetryCodecStatus::Ok
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
    Sht40Sensor climate_;
    BinaryReportSchedule reportSchedule_;
};

}  // namespace node
}  // namespace radiosensors
