#pragma once

// Serial diagnostics of NODE_DEBUG images, which share the release flash
// budget. Messages are short tags; the helpers stay out of line so a message
// costs one call instead of one per Serial.print.
#if defined(NODE_DEBUG)
#include <Arduino.h>
#include <stdint.h>

namespace radiosensors {
namespace node {

__attribute__((noinline)) inline void debugLine(const __FlashStringHelper *text) {
    Serial.println(text);
}

// Prints `tag` followed by `value` and ends the line.
__attribute__((noinline)) inline void debugValue(const __FlashStringHelper *tag,
                                                 const uint32_t value) {
    Serial.print(tag);
    Serial.println(value);
}

} // namespace node
} // namespace radiosensors
#endif
