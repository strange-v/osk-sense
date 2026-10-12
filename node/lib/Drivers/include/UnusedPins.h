#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace radiosensors {
namespace node {

// A pin with nothing attached floats toward mid-rail in power-down sleep,
// where both halves of its digital input buffer conduct. Disabling the buffer
// stops that current whatever level the pin floats to.
template <size_t Count> void disableUnusedPins(const uint8_t (&pins)[Count]) {
    for (const uint8_t pin : pins) {
        PORT_t *const port = digitalPinToPortStruct(pin);
        const uint8_t bitPosition = digitalPinToBitPosition(pin);
        port->DIRCLR = digitalPinToBitMask(pin);
        *getPINnCTRLregister(port, bitPosition) = PORT_ISC_INPUT_DISABLE_gc;
    }
}

} // namespace node
} // namespace radiosensors
