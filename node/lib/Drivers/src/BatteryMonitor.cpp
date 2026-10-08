#include "BatteryMonitor.h"

#include <Arduino.h>

namespace radiosensors {
namespace node {

uint16_t BatteryMonitor::readMillivolts() const {
    // LOWLAT must be set before configuring the ADC (silicon erratum).
    ADC0.CTRLA = ADC_LOWLAT_bm;
    // 500 kHz ADC clock at the 4 MHz CPU clock.
    ADC0.CTRLB = ADC_PRESC_DIV8_gc;
    ADC0.CTRLC = TIMEBASE_1US | ADC_REFSEL_1024MV_gc;
    ADC0.CTRLD = 0;
    // 25 us sampling, twice Microchip's VDDDIV10 example; Burst mode needs
    // SAMPDUR >= 12. 16 samples take 0.9 ms with noise below 1 mV.
    ADC0.CTRLE = 12;
    ADC0.CTRLF = ADC_SAMPNUM_ACC16_gc;
    ADC0.MUXPOS = ADC_MUXPOS_VDDDIV10_gc;
    ADC0.INTFLAGS = ADC_RESRDY_bm;
    ADC0.CTRLA = ADC_LOWLAT_bm | ADC_ENABLE_bm;
    ADC0.COMMAND = ADC_MODE_BURST_gc | ADC_START_IMMEDIATE_gc;
    while ((ADC0.INTFLAGS & ADC_RESRDY_bm) == 0) {}
    const uint32_t result = ADC0.RESULT;
    // LOWLAT keeps the reference powered; disable the whole ADC before sleep.
    ADC0.CTRLA = 0;
    // 16 x 12-bit samples of VDD/10 against 1024 mV, rounded to millivolts.
    return static_cast<uint16_t>((result * 10UL + 32UL) / 64UL);
}

}  // namespace node
}  // namespace radiosensors
