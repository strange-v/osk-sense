#pragma once

#include <stdint.h>

namespace radiosensors {
namespace node {

class BatteryMonitor {
  public:
    uint16_t readMillivolts() const;
};

} // namespace node
} // namespace radiosensors
