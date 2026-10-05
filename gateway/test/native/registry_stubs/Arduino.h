#pragma once
#include "../stubs/Arduino.h"
constexpr TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
TickType_t xTaskGetTickCount();
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return xSemaphoreCreateRecursiveMutex(); }
inline unsigned registryMutexTakes = 0;
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
    ++registryMutexTakes;
    return xSemaphoreTakeRecursive(mutex, wait);
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { xSemaphoreGiveRecursive(mutex); }
