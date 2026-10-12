#include "TwiBusRecovery.h"

#include <Arduino.h>
#include <I2cBusRecovery.h>

namespace radiosensors {
namespace node {

namespace {

// Drives a line only low, through DIR, and leaves OUT cleared: the TWI
// erratum needs OUT low on both pins, and the board's pull-ups make the high
// level.
class OpenDrainLine {
  public:
    explicit OpenDrainLine(const uint8_t pin)
        : port_(digitalPinToPortStruct(pin)), mask_(digitalPinToBitMask(pin)) {
        port_->OUTCLR = mask_;
    }

    void pullLow() {
        port_->DIRSET = mask_;
    }
    void release() {
        port_->DIRCLR = mask_;
    }
    bool high() const {
        return (port_->IN & mask_) != 0;
    }

  private:
    PORT_t *port_;
    uint8_t mask_;
};

class TwiPins {
  public:
    void pullSclLow() {
        scl_.pullLow();
    }
    void releaseScl() {
        scl_.release();
    }
    void pullSdaLow() {
        sda_.pullLow();
    }
    void releaseSda() {
        sda_.release();
    }
    bool sclHigh() const {
        return scl_.high();
    }
    bool sdaHigh() const {
        return sda_.high();
    }
    // About 100 kHz, standard mode, which every target accepts.
    void halfPeriod() {
        delayMicroseconds(5);
    }

  private:
    OpenDrainLine scl_{PIN_WIRE_SCL};
    OpenDrainLine sda_{PIN_WIRE_SDA};
};

} // namespace

bool recoverTwiBus(TwoWire &wire) {
    wire.end();
    TwiPins pins;
    const bool free = recoverI2cBus(pins);
    wire.begin();
    return free;
}

} // namespace node
} // namespace radiosensors
