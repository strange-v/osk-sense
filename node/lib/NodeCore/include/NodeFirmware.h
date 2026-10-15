#pragma once

#include <JoinRequest.h>

namespace radiosensors {
namespace node {

// One version for every node image; the gateway records it at pairing and
// from a ReadInfo command.
constexpr protocol::FirmwareVersion kFirmwareVersion{2, 0, 2};

} // namespace node
} // namespace radiosensors
