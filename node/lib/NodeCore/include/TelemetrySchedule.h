#pragma once

#include <stdint.h>

namespace radiosensors {
namespace node {

constexpr uint32_t kEventNodeKeepAliveMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kCounterMinimumReportMs = 60UL * 1000UL;

class ClimateReportPolicy {
  public:
    static ClimateReportPolicy fixed(const uint32_t intervalMs) {
        return ClimateReportPolicy(intervalMs, intervalMs, 0);
    }

    static ClimateReportPolicy adaptive(const uint32_t chargedIntervalMs,
                                        const uint32_t lowChargeIntervalMs,
                                        const uint16_t lowChargeThresholdMv) {
        return ClimateReportPolicy(chargedIntervalMs, lowChargeIntervalMs, lowChargeThresholdMv);
    }

    uint32_t intervalForMillivolts(const uint16_t supplyMillivolts) const {
        return supplyMillivolts > lowChargeThresholdMv_ ? chargedIntervalMs_ : lowChargeIntervalMs_;
    }

  private:
    ClimateReportPolicy(const uint32_t chargedIntervalMs,
                        const uint32_t lowChargeIntervalMs,
                        const uint16_t lowChargeThresholdMv)
        : chargedIntervalMs_(chargedIntervalMs), lowChargeIntervalMs_(lowChargeIntervalMs),
          lowChargeThresholdMv_(lowChargeThresholdMv) {
    }

    uint32_t chargedIntervalMs_;
    uint32_t lowChargeIntervalMs_;
    uint16_t lowChargeThresholdMv_;
};

inline bool intervalElapsed(const uint32_t now, const uint32_t since, const uint32_t interval) {
    return static_cast<uint32_t>(now - since) >= interval;
}

// Each report, acknowledged or not, schedules the next one on the interval
// for the supply voltage it measured, so a node that loses the gateway while
// its charge falls still slows down.
class ClimateReportSchedule {
  public:
    explicit ClimateReportSchedule(const ClimateReportPolicy policy) : policy_(policy) {
    }

    bool due(const uint32_t now) const {
        return !hasAttempt_ || intervalElapsed(now, lastAttempt_, intervalMs_);
    }

    // Returns the interval until the next report.
    uint32_t attempted(const uint32_t now, const uint16_t supplyMillivolts) {
        intervalMs_ = policy_.intervalForMillivolts(supplyMillivolts);
        lastAttempt_ = now;
        hasAttempt_ = true;
        return intervalMs_;
    }

  private:
    ClimateReportPolicy policy_;
    uint32_t intervalMs_ = 0;
    uint32_t lastAttempt_ = 0;
    bool hasAttempt_ = false;
};

class RollingKeepAlive {
  public:
    explicit RollingKeepAlive(const uint32_t intervalMs) : intervalMs_(intervalMs) {
    }

    bool due(const uint32_t now) const {
        return !hasSuccessfulTransmission_ ||
               intervalElapsed(now, lastSuccessfulTransmission_, intervalMs_);
    }

    void transmissionSucceeded(const uint32_t now) {
        lastSuccessfulTransmission_ = now;
        hasSuccessfulTransmission_ = true;
    }

    void setInterval(const uint32_t intervalMs) {
        intervalMs_ = intervalMs;
    }

  private:
    uint32_t intervalMs_;
    uint32_t lastSuccessfulTransmission_ = 0;
    bool hasSuccessfulTransmission_ = false;
};

class BinaryReportSchedule {
  public:
    explicit BinaryReportSchedule(const uint32_t intervalMs = kEventNodeKeepAliveMs)
        : keepAlive_(intervalMs) {
    }

    void stateChanged() {
        stateChanged_ = true;
        urgent_ = true;
    }

    bool due(const uint32_t now) const {
        return stateChanged_ || keepAlive_.due(now);
    }

    // True once per state change: the first attempt to report a new state
    // ignores radio retry backoff, later retries of the same report do not.
    bool takeUrgent() {
        const bool urgent = urgent_;
        urgent_ = false;
        return urgent;
    }

    void transmissionSucceeded(const uint32_t now) {
        stateChanged_ = false;
        keepAlive_.transmissionSucceeded(now);
    }

  private:
    RollingKeepAlive keepAlive_;
    bool stateChanged_ = false;
    bool urgent_ = false;
};

class CounterReportSchedule {
  public:
    CounterReportSchedule() : keepAlive_(kEventNodeKeepAliveMs) {
    }

    void pulseRecorded() {
        countChanged_ = true;
    }

    void countSet() {
        countChanged_ = true;
        immediateReportPending_ = true;
        urgent_ = true;
    }

    bool takeUrgent() {
        const bool urgent = urgent_;
        urgent_ = false;
        return urgent;
    }

    bool due(const uint32_t now) const {
        return immediateReportPending_ || keepAlive_.due(now) ||
               (countChanged_ && hasSuccessfulTransmission_ &&
                intervalElapsed(now, lastSuccessfulTransmission_, kCounterMinimumReportMs));
    }

    void transmissionSucceeded(const uint32_t now) {
        countChanged_ = false;
        immediateReportPending_ = false;
        lastSuccessfulTransmission_ = now;
        hasSuccessfulTransmission_ = true;
        keepAlive_.transmissionSucceeded(now);
    }

  private:
    RollingKeepAlive keepAlive_;
    uint32_t lastSuccessfulTransmission_ = 0;
    bool hasSuccessfulTransmission_ = false;
    bool countChanged_ = false;
    bool immediateReportPending_ = false;
    bool urgent_ = false;
};

// Delays telemetry after a report went unacknowledged: 1, 5, then every 15
// minutes, and hourly once the gateway has been silent for a day, which spares
// the battery of a node whose gateway is gone for good.
class RadioRetryBackoff {
  public:
    // For an image whose report schedule already paces its retries.
    static RadioRetryBackoff none() {
        return RadioRetryBackoff(false);
    }

    RadioRetryBackoff() : RadioRetryBackoff(true) {
    }

    bool allowed(const uint32_t now) const {
        return !enabled_ || !waiting_ || intervalElapsed(now, failedAt_, delayMs_);
    }

    void failed(const uint32_t now) {
        static constexpr uint32_t delays[] = {
            60UL * 1000UL,
            5UL * 60UL * 1000UL,
            15UL * 60UL * 1000UL,
        };
        static constexpr uint8_t kLastLevel = sizeof(delays) / sizeof(delays[0]) - 1;
        static constexpr uint32_t kSilentDayMs = 24UL * 60UL * 60UL * 1000UL;
        static constexpr uint32_t kSilentDayDelayMs = 60UL * 60UL * 1000UL;
        if (!waiting_) {
            firstFailedAt_ = now;
            failureLevel_ = 0;
            silentForDay_ = false;
        } else {
            if (failureLevel_ < kLastLevel)
                ++failureLevel_;
            // Latched: the elapsed time wraps after 49 days.
            if (intervalElapsed(now, firstFailedAt_, kSilentDayMs))
                silentForDay_ = true;
        }
        failedAt_ = now;
        delayMs_ = silentForDay_ ? kSilentDayDelayMs : delays[failureLevel_];
        waiting_ = true;
    }

    void succeeded() {
        waiting_ = false;
    }

  private:
    explicit RadioRetryBackoff(const bool enabled) : enabled_(enabled) {
    }

    uint32_t firstFailedAt_ = 0;
    uint32_t failedAt_ = 0;
    uint32_t delayMs_ = 0;
    uint8_t failureLevel_ = 0;
    bool enabled_;
    bool waiting_ = false;
    bool silentForDay_ = false;
};

// Limits command sessions that a telemetry acknowledgement starts. A gateway
// that keeps announcing a command it never records would otherwise cost a
// session per report: at most one per five minutes, and after one the gateway
// did not answer, the radio retry delays.
class HintedSessionPolicy {
  public:
    static constexpr uint32_t kMinimumIntervalMs = 5UL * 60UL * 1000UL;

    bool allowed(const uint32_t now) const {
        return (!hasSession_ || intervalElapsed(now, lastSession_, kMinimumIntervalMs)) &&
               unanswered_.allowed(now);
    }

    void finished(const uint32_t now, const bool answered) {
        lastSession_ = now;
        hasSession_ = true;
        if (answered)
            unanswered_.succeeded();
        else
            unanswered_.failed(now);
    }

  private:
    RadioRetryBackoff unanswered_;
    uint32_t lastSession_ = 0;
    bool hasSession_ = false;
};

// Commissioning attempts: at boot and on every button press, then every five
// minutes for twelve attempts in all. After that a provisional node retries
// hourly, because the gateway answers its Join confirm at any time; an
// unconfigured node waits for the button, because the gateway accepts a Join
// request only inside a pairing window.
class JoinRetrySchedule {
  public:
    static constexpr uint32_t kBurstIntervalMs = 5UL * 60UL * 1000UL;
    static constexpr uint8_t kBurstAttempts = 12;
    static constexpr uint32_t kProvisionalIntervalMs = 60UL * 60UL * 1000UL;

    bool due(const uint32_t now, const bool provisional) const {
        if (!attempted_)
            return true;
        if (burstAttempts_ < kBurstAttempts)
            return intervalElapsed(now, lastAttempt_, kBurstIntervalMs);
        return provisional && intervalElapsed(now, lastAttempt_, kProvisionalIntervalMs);
    }

    void attempted(const uint32_t now, const bool requestedByButton) {
        if (requestedByButton)
            burstAttempts_ = 0;
        if (burstAttempts_ < kBurstAttempts)
            ++burstAttempts_;
        lastAttempt_ = now;
        attempted_ = true;
    }

  private:
    uint32_t lastAttempt_ = 0;
    uint8_t burstAttempts_ = 0;
    bool attempted_ = false;
};

} // namespace node
} // namespace radiosensors
