#include "RtcEntropy.h"
#include "PairingEntropy.h"
#include <Arduino.h>
#include <avr/interrupt.h>

#if defined(MILLIS_USE_TIMERB0) || defined(MILLIS_USE_TIMERRTC)
#error "RTC entropy capture needs TCB0 and RTC independently of millis()"
#endif

namespace radiosensors { namespace node {
namespace {
bool synchronized() {
    uint16_t budget = UINT16_MAX;
    while (RTC.STATUS || RTC.PITSTATUS) if (--budget == 0) return false;
    return true;
}
class Capture {
public:
    Capture() : pit_(RTC.PITCTRLA), pitInterrupt_(RTC.PITINTCTRL), clock_(RTC.CLKSEL),
        rtc_(RTC.CTRLA), rtcInterrupt_(RTC.INTCTRL), period_(RTC.PER), count_(RTC.CNT),
        channel_(EVSYS.CHANNEL0), user_(EVSYS.USERTCB0CAPT),
        timer_(TCB0.CTRLA), mode_(TCB0.CTRLB), event_(TCB0.EVCTRL),
        interrupt_(TCB0.INTCTRL), timerCount_(TCB0.CNT), compare_(TCB0.CCMP) {}
    bool begin() {
        if (!synchronized()) return false;
        // Changing the RTC source also changes PIT frequency. Pause its ticks
        // for this burst, then restore the sleep clock before returning.
        RTC.PITINTCTRL = 0; RTC.PITCTRLA = 0; RTC.INTCTRL = 0; RTC.CTRLA = 0;
        if (!synchronized()) return false;
        RTC.CLKSEL = RTC_CLKSEL_INT32K_gc; RTC.PER = 63; RTC.CNT = 0;
        if (!synchronized()) return false;
        TCB0.CTRLA = 0;
        EVSYS.CHANNEL0 = EVSYS_CHANNEL0_RTC_OVF_gc;
        EVSYS.USERTCB0CAPT = EVSYS_USER_CHANNEL0_gc;
        TCB0.CTRLB = TCB_CNTMODE_FRQ_gc; TCB0.EVCTRL = TCB_CAPTEI_bm;
        TCB0.INTCTRL = 0; TCB0.INTFLAGS = TCB_CAPT_bm;
        TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
        RTC.CTRLA = RTC_PRESCALER_DIV1_gc | RTC_RTCEN_bm;
        uint16_t ignored;
        return next(ignored) && next(ignored);
    }
    bool next(uint16_t& value) {
        const uint32_t started = millis();
        uint16_t budget = UINT16_MAX;
        while (!(TCB0.INTFLAGS & TCB_CAPT_bm)) {
            if (--budget == 0 || static_cast<uint32_t>(millis() - started) >= 8) return false;
        }
        value = TCB0.CCMP; TCB0.INTFLAGS = TCB_CAPT_bm;
        return true;
    }
    ~Capture() {
        const uint8_t saved = SREG; cli();
        TCB0.CTRLA = 0;
        TCB0.CTRLB = mode_; TCB0.EVCTRL = event_; TCB0.INTCTRL = interrupt_;
        TCB0.CNT = timerCount_; TCB0.CCMP = compare_; TCB0.INTFLAGS = TCB_CAPT_bm;
        EVSYS.CHANNEL0 = channel_; EVSYS.USERTCB0CAPT = user_; TCB0.CTRLA = timer_;
        RTC.CTRLA = 0; synchronized();
        RTC.CLKSEL = clock_; RTC.PER = period_; RTC.CNT = count_; synchronized();
        RTC.CTRLA = rtc_; RTC.INTCTRL = rtcInterrupt_;
        RTC.PITINTFLAGS = RTC_PI_bm; RTC.PITCTRLA = pit_; RTC.PITINTCTRL = pitInterrupt_;
        SREG = saved;
    }
private:
    uint8_t pit_, pitInterrupt_, clock_, rtc_, rtcInterrupt_;
    uint16_t period_, count_;
    uint8_t channel_, user_, timer_, mode_, event_, interrupt_;
    uint16_t timerCount_, compare_;
};
}
bool collectRtcEntropy(uint8_t (&material)[8]) {
    Capture capture;
    return capture.begin() && pairing_entropy::collect(capture,material);
}
} }
