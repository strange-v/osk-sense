#include "LowPowerClock.h"

#include <Arduino.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

namespace {
#if NODE_TICK_MS == 32000
constexpr uint8_t kPitPeriod = RTC_PERIOD_CYC32768_gc;
#elif NODE_TICK_MS == 250
constexpr uint8_t kPitPeriod = RTC_PERIOD_CYC256_gc;
#else
#error "NODE_TICK_MS must be an exact 1024 Hz RTC PIT period: 32000 or 250"
#endif

volatile uint32_t elapsedMs = 0;
} // namespace

ISR(RTC_PIT_vect) {
    RTC.PITINTFLAGS = RTC_PI_bm;
    elapsedMs += radiosensors::node::LowPowerClock::kTickMs;
}

namespace radiosensors {
namespace node {

void LowPowerClock::begin() {
    ADC0.CTRLA &= ~ADC_ENABLE_bm;
#if !defined(NODE_DEBUG)
    TCA0.SPLIT.CTRLA &= ~TCA_SPLIT_ENABLE_bm;
#endif
    while (RTC.STATUS != 0) {
    }
    RTC.CLKSEL = RTC_CLKSEL_INT1K_gc;
    RTC.PITINTCTRL = RTC_PI_bm;
    RTC.PITCTRLA = kPitPeriod | RTC_PITEN_bm;
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
}

uint32_t LowPowerClock::nowMs() const {
    const uint8_t status = SREG;
    cli();
    const uint32_t value = elapsedMs;
    SREG = status;
    return value;
}

void LowPowerClock::sleepUntilInterrupt() const {
#if defined(NODE_DEBUG)
    delay(NODE_DEBUG_IDLE_DELAY_MS);
#else
    sleep_cpu();
#endif
}

} // namespace node
} // namespace radiosensors
