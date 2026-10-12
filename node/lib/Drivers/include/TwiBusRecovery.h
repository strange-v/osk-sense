#pragma once

#include <Wire.h>

namespace radiosensors {
namespace node {

// Stops TWI0, frees its bus from a target stuck mid-byte, and starts TWI0
// again. On a bus that was not stuck this costs tens of microseconds.
// Returns whether both lines were high before the restart.
bool recoverTwiBus(TwoWire &wire);

} // namespace node
} // namespace radiosensors
