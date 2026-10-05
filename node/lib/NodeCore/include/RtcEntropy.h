#pragma once
#include <stdint.h>
namespace radiosensors { namespace node {
// Returns only material that passes PairingEntropy's whole-burst checks.
bool collectRtcEntropy(uint8_t (&material)[8]);
} }
