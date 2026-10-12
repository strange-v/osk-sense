#include "PasswordHashService.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace gateway::password_hash {
namespace {

constexpr uint32_t kWorkerStackSize = 4096;
constexpr UBaseType_t kWorkerPriority = 1;
constexpr BaseType_t kWorkerCore = ARDUINO_RUNNING_CORE;
constexpr TickType_t kWatchdogFeedInterval = pdMS_TO_TICKS(1000);

struct Job {
    const char *password;
    size_t passwordLength;
    const uint8_t *salt;
    size_t saltLength;
    uint32_t iterations;
    uint8_t *output;
    size_t outputLength;
    SemaphoreHandle_t completed;
    int result;
};

SemaphoreHandle_t workerMutex = nullptr;

void run(void *context) {
    auto &job = *static_cast<Job *>(context);
    job.result =
        mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                      reinterpret_cast<const unsigned char *>(job.password),
                                      job.passwordLength,
                                      job.salt,
                                      job.saltLength,
                                      job.iterations,
                                      job.outputLength,
                                      job.output);
    xSemaphoreGive(job.completed);
    vTaskDelete(nullptr);
}

} // namespace

bool begin() {
    if (workerMutex != nullptr)
        return true;
    workerMutex = xSemaphoreCreateMutex();
    return workerMutex != nullptr;
}

bool computePbkdf2Sha256(const char *const password,
                         const size_t passwordLength,
                         const uint8_t *const salt,
                         const size_t saltLength,
                         const uint32_t iterations,
                         uint8_t *const output,
                         const size_t outputLength) {
    if (workerMutex == nullptr || password == nullptr || salt == nullptr || output == nullptr ||
        iterations == 0 || outputLength == 0 ||
        xSemaphoreTake(workerMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    SemaphoreHandle_t completed = xSemaphoreCreateBinary();
    if (completed == nullptr) {
        xSemaphoreGive(workerMutex);
        return false;
    }

    Job job{
        password,
        passwordLength,
        salt,
        saltLength,
        iterations,
        output,
        outputLength,
        completed,
        -1,
    };
    const BaseType_t created = xTaskCreatePinnedToCore(run,
                                                       "password_hash",
                                                       kWorkerStackSize,
                                                       &job,
                                                       kWorkerPriority,
                                                       nullptr,
                                                       kWorkerCore);
    if (created != pdPASS) {
        vSemaphoreDelete(completed);
        xSemaphoreGive(workerMutex);
        return false;
    }

    while (xSemaphoreTake(completed, kWatchdogFeedInterval) != pdTRUE) {
        // Password requests run in AsyncTCP's task. It is subscribed to the
        // task watchdog, so keep its bounded wait alive while the low-priority
        // worker lets the Arduino loop and network stack continue running.
        esp_task_wdt_reset();
    }
    esp_task_wdt_reset();
    vSemaphoreDelete(completed);
    xSemaphoreGive(workerMutex);
    return job.result == 0;
}

} // namespace gateway::password_hash
