#include "PolledReedInput.h"

namespace radiosensors {
namespace node {

void PolledReedInput::sleep() {
    // Preload the output latch before enabling the output driver. With the
    // contact connected to ground, OUTPUT LOW creates no DC path through it.
    digitalWrite(pin_, LOW);
    pinMode(pin_, OUTPUT);
}

bool PolledReedInput::readOnce() {
    pinMode(pin_, INPUT_PULLUP);
    delayMicroseconds(kSettleMicroseconds);
    const bool high = digitalRead(pin_) == HIGH;
    sleep();
    return high;
}

bool PolledReedInput::sample(bool &rawHigh) {
    pinMode(pin_, INPUT_PULLUP);
    delayMicroseconds(kSettleMicroseconds);

    bool candidate = digitalRead(pin_) == HIGH;
    uint8_t equalSamples = 1;
    uint8_t totalSamples = 1;
    while (equalSamples < kStableSamples && totalSamples < kMaximumSamples) {
        delay(kSampleSpacingMilliseconds);
        const bool current = digitalRead(pin_) == HIGH;
        ++totalSamples;
        if (current == candidate) {
            ++equalSamples;
        } else {
            candidate = current;
            equalSamples = 1;
        }
    }

    sleep();
    if (equalSamples < kStableSamples)
        return false;
    rawHigh = candidate;
    return true;
}

} // namespace node
} // namespace radiosensors
