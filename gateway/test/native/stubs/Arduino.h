#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mutex>
using TickType_t = uint32_t;
using SemaphoreHandle_t = std::recursive_mutex *;
constexpr auto portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    return new std::recursive_mutex;
}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
    mutex->lock();
    return pdTRUE;
}
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
    mutex->unlock();
}
uint32_t millis();
struct TestEsp {
    void restart();
};
extern TestEsp ESP;
struct TestSerial {
    void println(const char *) {
    }
    template <typename... Arguments> void printf(const char *, Arguments...) {
    }
    void flush() {
    }
};
inline TestSerial Serial;
