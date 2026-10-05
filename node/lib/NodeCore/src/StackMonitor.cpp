#include "StackMonitor.h"

#if defined(NODE_STACK_DIAGNOSTICS)
#include <avr/io.h>
#include <stdint.h>
#include "DebugLog.h"

extern "C" {
extern uint8_t __heap_start;
extern void* __brkval;

// Paint unused SRAM before constructors can use the stack. Naked init sections
// fall through to startup; basic assembly avoids a compiler-generated prologue.
void paintStack() __attribute__((naked, used, section(".init3")));
void paintStack() {
    asm volatile(
        "ldi r30, lo8(__heap_start)\n"
        "ldi r31, hi8(__heap_start)\n"
        "in r24, __SP_L__\n"
        "in r25, __SP_H__\n"
        "ldi r18, 0xa5\n"
        "1: cp r30, r24\n"
        "cpc r31, r25\n"
        "brsh 2f\n"
        "st Z+, r18\n"
        "rjmp 1b\n"
        "2:\n");
}
}

namespace radiosensors::node::stack_monitor {
void report() {
    static uint16_t minimum = UINT16_MAX;
    const auto* end = reinterpret_cast<const uint8_t*>(SP);
    const volatile uint8_t* cursor = __brkval
        ? static_cast<const uint8_t*>(__brkval) : &__heap_start;
    uint16_t freeBytes = 0;
    while (cursor < end && *cursor == 0xa5) { ++cursor; ++freeBytes; }
    if (freeBytes < minimum) {
        minimum = freeBytes;
        debugValue(F("stkfree "), minimum);
    }
}
}  // namespace radiosensors::node::stack_monitor
#endif
