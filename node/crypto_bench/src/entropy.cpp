#include <Arduino.h>
#include <avr/interrupt.h>

namespace {
uint16_t samples[128];
uint16_t edge() {
    while ((TCB0.INTFLAGS & TCB_CAPT_bm) == 0) {}
    const uint16_t value = TCB0.CCMP;
    TCB0.INTFLAGS = TCB_CAPT_bm;
    return value;
}
}

void setup() {
    Serial.begin(9600);
    while (RTC.STATUS != 0) {}
    RTC.CLKSEL = RTC_CLKSEL_INT32K_gc;
    RTC.PER = 63;
    RTC.CTRLA = RTC_PRESCALER_DIV1_gc | RTC_RTCEN_bm;
    TCB0.CTRLA = 0;
    EVSYS.CHANNEL0 = EVSYS_CHANNEL0_RTC_OVF_gc;
    EVSYS.USERTCB0CAPT = EVSYS_USER_CHANNEL0_gc;
    TCB0.CTRLB = TCB_CNTMODE_FRQ_gc;
    TCB0.EVCTRL = TCB_CAPTEI_bm;
    TCB0.INTCTRL = 0;
    TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
    Serial.println(F("RTC32K DIV64 MAIN4M CAPTURE"));
}

void loop() {
    Serial.flush();
    const uint8_t saved = SREG;
    cli();
    edge();
    for (uint8_t i = 0; i < 128; ++i) {
        samples[i] = edge();
    }
    SREG = saved;
    for (uint8_t i = 0; i < 128; ++i) Serial.println(samples[i]);
}
