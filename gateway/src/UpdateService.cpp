#include "UpdateService.h"

#include <Update.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>

#include "BackupService.h"
#include "BoardProfile.h"
#include "EthernetService.h"
#include "FirmwareVersion.h"
#include "GzipInflater.h"
#include "OtaService.h"
#include "RecoveryService.h"
#include "TimeService.h"
#include "WebUiService.h"

// Arduino confirms an image on trial during startup unless this returns true;
// update::loop() confirms it only once the gateway is reachable again.
extern "C" bool verifyRollbackLater() {
    return true;
}

namespace gateway::update {
namespace {

constexpr char kReleases[] = "https://github.com/strange-v/RadioSensors/releases";
constexpr uint32_t kTaskStackSize = 12288;
// Below the Arduino loop and on the other core from it and the radio task, so
// TLS handshakes never delay telemetry or the watchdog-guarded loop.
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;
constexpr int kHttpTimeoutMs = 20000;
// GitHub redirects downloads to signed storage URLs of about 1 KB.
constexpr int kHttpBufferSize = 4096;
constexpr uint8_t kMaxRedirects = 5;
constexpr size_t kChunkSize = 2048;
constexpr size_t kSectorSize = 4096;
// An image on trial is confirmed after this long on the network; a crash
// before then makes the bootloader return to the previous image.
constexpr uint32_t kConfirmAfterMs = 60000;
// Without Ethernet for this long the image is rejected: a gateway that cannot
// reach the network cannot be updated again.
constexpr uint32_t kRejectAfterMs = 600000;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
Status current;
release::Release available;
std::atomic<bool> taskRunning{false};
bool imageOnTrial = false;

void setState(const State state, const char *error = "") {
    portENTER_CRITICAL(&statusMux);
    current.state = state;
    current.error = error;
    if (state != State::Installing)
        current.progress = 0;
    portEXIT_CRITICAL(&statusMux);
}

void fail(const char *error) {
    Serial.printf("Update failed: %s\n", error);
    setState(State::Failed, error);
}

using Sink = bool (*)(void *context, const uint8_t *data, size_t size);

// Streams `url` into `sink`, following redirects. Returns an error code, or
// nullptr once exactly the response body has been delivered.
const char *download(const char *url, const size_t maxSize, Sink sink, void *context) {
    esp_http_client_config_t config{};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;
    config.user_agent = "osk-sense-gateway";
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
        return "http_init";

    const char *error = "download_failed";
    int status = 0;
    int64_t length = -1;
    for (uint8_t redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        if (esp_http_client_open(client, 0) != ESP_OK)
            break;
        length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
            break;
        esp_http_client_flush_response(client, nullptr);
        const bool redirected = esp_http_client_set_redirection(client) == ESP_OK;
        esp_http_client_close(client);
        if (!redirected) {
            status = 0;
            break;
        }
    }

    if (status == 404) {
        error = "not_found";
    } else if (status == 200) {
        if (length > static_cast<int64_t>(maxSize)) {
            error = "too_large";
        } else {
            std::unique_ptr<uint8_t[]> chunk(new (std::nothrow) uint8_t[kChunkSize]);
            size_t total = 0;
            error = chunk ? nullptr : "out_of_memory";
            while (error == nullptr) {
                const int read =
                    esp_http_client_read(client, reinterpret_cast<char *>(chunk.get()), kChunkSize);
                if (read < 0)
                    error = "download_failed";
                if (read <= 0)
                    break;
                total += static_cast<size_t>(read);
                if (total > maxSize)
                    error = "too_large";
                else if (!sink(context, chunk.get(), static_cast<size_t>(read)))
                    error = "write_failed";
            }
            if (error == nullptr && !esp_http_client_is_complete_data_received(client))
                error = "download_failed";
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return error;
}

struct Buffer {
    uint8_t *data;
    size_t capacity;
    size_t size;
};

bool appendToBuffer(void *context, const uint8_t *data, const size_t size) {
    auto *buffer = static_cast<Buffer *>(context);
    if (size > buffer->capacity - buffer->size)
        return false;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return true;
}

const char *networkError() {
    if (!ethernet::hasIp())
        return "network_unavailable";
    // Certificate validity cannot be checked before the clock is set.
    if (time_service::state() != time_service::State::Synchronized)
        return "time_not_synchronized";
    return nullptr;
}

// Fetches and verifies manifest.signed from `path` ("latest/download" or
// "download/<version>"). Returns an error code, or nullptr with `status` set.
const char *fetchManifest(const char *path,
                          const char *currentVersion,
                          release::Status &status,
                          release::Release &result) {
    std::unique_ptr<uint8_t[]> manifest(new (std::nothrow)
                                            uint8_t[release::kMaxSignedManifestSize]);
    if (!manifest)
        return "out_of_memory";
    Buffer buffer{manifest.get(), release::kMaxSignedManifestSize, 0};
    char url[128];
    snprintf(url, sizeof(url), "%s/%s/manifest.signed", kReleases, path);
    if (const char *error = download(url, buffer.capacity, appendToBuffer, &buffer))
        return error;
    status = release::readSigned(buffer.data,
                                 buffer.size,
                                 board::current.releaseId,
                                 currentVersion,
                                 result);
    return nullptr;
}

void check() {
    if (const char *error = networkError())
        return fail(error);

    release::Status status{};
    release::Release result;
    if (const char *error = fetchManifest("latest/download", firmware::version, status, result))
        return fail(error);
    if (status == release::Status::NotNewer) {
        Serial.println("Update check: firmware is current");
        return setState(State::UpToDate);
    }
    if (status != release::Status::Ok)
        return fail(release::statusName(status));

    available = result;
    portENTER_CRITICAL(&statusMux);
    memcpy(current.availableVersion, result.versionText, sizeof(current.availableVersion));
    portEXIT_CRITICAL(&statusMux);
    Serial.printf("Update check: %s available\n", result.versionText);
    setState(State::Available);
}

// Download progress across both images of an install.
size_t progressDone = 0;
size_t progressTotal = 1;

void advanceProgress(const size_t bytes) {
    progressDone += bytes;
    const uint8_t progress =
        static_cast<uint8_t>((static_cast<uint64_t>(progressDone) * 100U) / progressTotal);
    portENTER_CRITICAL(&statusMux);
    current.progress = progress;
    portEXIT_CRITICAL(&statusMux);
}

struct FirmwareWriter {
    mbedtls_sha256_context hash;
    size_t written;
};

bool writeFirmware(void *context, const uint8_t *data, const size_t size) {
    auto *writer = static_cast<FirmwareWriter *>(context);
    mbedtls_sha256_update(&writer->hash, data, size);
    if (Update.write(const_cast<uint8_t *>(data), size) != size)
        return false;
    writer->written += size;
    advanceProgress(size);
    return true;
}

const char *installFirmware() {
    const release::Image &image = available.firmware;
    if (!Update.begin(image.size, U_FLASH))
        return "flash_begin_failed";

    FirmwareWriter writer{};
    mbedtls_sha256_init(&writer.hash);
    mbedtls_sha256_starts(&writer.hash, 0);
    char url[192];
    snprintf(url, sizeof(url), "%s/download/%s/%s", kReleases, available.versionText, image.file);
    Serial.printf("Update: downloading %s\n", url);
    const char *error = download(url, image.size, writeFirmware, &writer);
    uint8_t digest[release::kSha256Size];
    mbedtls_sha256_finish(&writer.hash, digest);
    mbedtls_sha256_free(&writer.hash);

    if (error == nullptr && writer.written != image.size)
        error = "size_mismatch";
    if (error == nullptr && memcmp(digest, image.sha256, sizeof(digest)) != 0)
        error = "hash_mismatch";
    // Only a complete image with the signed hash becomes the boot partition.
    if (error != nullptr) {
        Update.abort();
        return error;
    }
    return Update.end() ? nullptr : "image_invalid";
}

// Writes the inflated Web UI image over the `web` partition one sector at a
// time. A sector that already holds the new bytes is left alone, so the mostly
// empty partition costs a read rather than an erase.
struct UiWriter {
    const esp_partition_t *partition;
    uint8_t *sector;
    uint8_t *existing;
    size_t fill;
    size_t offset;
    mbedtls_sha256_context hash;
    gzip::Inflater inflater;
};

bool erased(const uint8_t *data, const size_t size) {
    for (size_t index = 0; index < size; ++index)
        if (data[index] != 0xff)
            return false;
    return true;
}

bool commitSector(UiWriter &writer) {
    const size_t address = writer.offset;
    if (esp_partition_read(writer.partition, address, writer.existing, kSectorSize) != ESP_OK)
        return false;
    if (memcmp(writer.existing, writer.sector, kSectorSize) != 0 &&
        (esp_partition_erase_range(writer.partition, address, kSectorSize) != ESP_OK ||
         (!erased(writer.sector, kSectorSize) &&
          esp_partition_write(writer.partition, address, writer.sector, kSectorSize) != ESP_OK))) {
        return false;
    }
    writer.offset += kSectorSize;
    writer.fill = 0;
    return true;
}

bool writeUiImage(void *context, const uint8_t *data, size_t size) {
    auto &writer = *static_cast<UiWriter *>(context);
    mbedtls_sha256_update(&writer.hash, data, size);
    while (size > 0) {
        if (writer.offset >= writer.partition->size)
            return false;
        const size_t chunk = std::min(size, kSectorSize - writer.fill);
        memcpy(writer.sector + writer.fill, data, chunk);
        writer.fill += chunk;
        data += chunk;
        size -= chunk;
        if (writer.fill == kSectorSize && !commitSector(writer))
            return false;
    }
    return true;
}

bool receiveUi(void *context, const uint8_t *data, const size_t size) {
    advanceProgress(size);
    return static_cast<UiWriter *>(context)->inflater.write(data, size);
}

const char *installUi() {
    const release::Image &image = available.ui;
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "web");
    if (partition == nullptr)
        return "ui_partition_missing";
    // The image is built for this board's partition, so the sizes match exactly.
    if (image.imageSize != partition->size || partition->size % kSectorSize != 0)
        return "ui_size_mismatch";
    std::unique_ptr<uint8_t[]> sector(new (std::nothrow) uint8_t[kSectorSize]);
    std::unique_ptr<uint8_t[]> existing(new (std::nothrow) uint8_t[kSectorSize]);
    UiWriter writer{};
    writer.partition = partition;
    writer.sector = sector.get();
    writer.existing = existing.get();
    if (!sector || !existing ||
        !writer.inflater.begin(release::kUiWindowSize, writeUiImage, &writer)) {
        return "out_of_memory";
    }
    mbedtls_sha256_init(&writer.hash);
    mbedtls_sha256_starts(&writer.hash, 0);

    char url[192];
    snprintf(url, sizeof(url), "%s/download/%s/%s", kReleases, available.versionText, image.file);
    Serial.printf("Update: downloading %s\n", url);
    web_ui::prepareForFilesystemUpdate();
    const char *error = download(url, image.size, receiveUi, &writer);
    uint8_t digest[release::kSha256Size];
    mbedtls_sha256_finish(&writer.hash, digest);
    mbedtls_sha256_free(&writer.hash);

    if (error == nullptr &&
        (!writer.inflater.finish() || writer.fill != 0 || writer.offset != image.imageSize)) {
        error = "size_mismatch";
    }
    if (error == nullptr && memcmp(digest, image.sha256, sizeof(digest)) != 0)
        error = "hash_mismatch";
    // A failed image leaves the recovery page, which can install it again.
    if (error != nullptr)
        web_ui::recoverAfterFailedFilesystemUpdate();
    return error;
}

void install() {
    if (const char *error = networkError())
        return fail(error);
    progressDone = 0;
    progressTotal = available.ui.size + available.firmware.size;
    // The Web UI first: if the firmware then fails, the recovery page of the
    // running firmware can reinstall a matching UI.
    if (const char *error = installUi())
        return fail(error);
    if (const char *error = installFirmware()) {
        web_ui::recoverAfterFailedFilesystemUpdate();
        return fail(error);
    }

    Serial.printf("Update: %s installed; restarting\n", available.versionText);
    setState(State::Restarting);
    recovery::restartSoon();
}

// Reinstalls the Web UI published with the running firmware.
void repairUi() {
    if (const char *error = networkError())
        return fail(error);

    char path[48];
    snprintf(path, sizeof(path), "download/%s", firmware::version);
    release::Status status{};
    release::Release result;
    // Any real version is newer than 0.0.0, so the running one reads as Ok.
    if (const char *error = fetchManifest(path, "0.0.0", status, result))
        return fail(error);
    if (status != release::Status::Ok)
        return fail(release::statusName(status));
    if (strcmp(result.versionText, firmware::version) != 0)
        return fail("version_mismatch");

    available = result;
    progressDone = 0;
    progressTotal = available.ui.size;
    if (const char *error = installUi())
        return fail(error);
    // The static asset route is registered at startup, so serve the new UI
    // after a restart rather than by remounting.
    Serial.println("Update: Web UI reinstalled; restarting");
    setState(State::Restarting);
    recovery::restartSoon();
}

void repairTask(void *) {
    repairUi();
    taskRunning.store(false);
    vTaskDelete(nullptr);
}

void checkTask(void *) {
    check();
    taskRunning.store(false);
    vTaskDelete(nullptr);
}

void installTask(void *) {
    install();
    taskRunning.store(false);
    vTaskDelete(nullptr);
}

bool startTask(TaskFunction_t function, const char *name, const State state) {
    bool expected = false;
    if (!taskRunning.compare_exchange_strong(expected, true))
        return false;
    setState(state);
    if (xTaskCreatePinnedToCore(function,
                                name,
                                kTaskStackSize,
                                nullptr,
                                kTaskPriority,
                                nullptr,
                                kTaskCore) != pdPASS) {
        taskRunning.store(false);
        fail("task_failed");
        return false;
    }
    return true;
}

} // namespace

void begin() {
    esp_ota_img_states_t state{};
    imageOnTrial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    portENTER_CRITICAL(&statusMux);
    current.pendingVerify = imageOnTrial;
    portEXIT_CRITICAL(&statusMux);
    if (imageOnTrial)
        Serial.println("Firmware image on trial: confirming once the network is up");
}

void loop() {
    if (!imageOnTrial)
        return;
    const uint32_t now = millis();
    if (now >= kConfirmAfterMs && ethernet::hasIp()) {
        imageOnTrial = false;
        const bool confirmed = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
        portENTER_CRITICAL(&statusMux);
        current.pendingVerify = !confirmed;
        portEXIT_CRITICAL(&statusMux);
        Serial.println(confirmed ? "Firmware image confirmed"
                                 : "Firmware image confirmation failed");
    } else if (now >= kRejectAfterMs) {
        Serial.println("Firmware image rejected: no network; returning to the previous image");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

bool startCheck() {
    if (recovery::blocked())
        return false;
    return startTask(checkTask, "update-check", State::Checking);
}

bool startInstall() {
    if (recovery::blocked() || backup::busy() || ota::state() == ota::State::Updating ||
        status().state != State::Available) {
        return false;
    }
    return startTask(installTask, "update-install", State::Installing);
}

bool startUiRepair() {
    if (recovery::blocked() || backup::busy() || ota::state() == ota::State::Updating)
        return false;
    return startTask(repairTask, "update-ui", State::Installing);
}

Status status() {
    portENTER_CRITICAL(&statusMux);
    const Status result = current;
    portEXIT_CRITICAL(&statusMux);
    return result;
}

const char *stateName(const State state) {
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Checking:
        return "checking";
    case State::UpToDate:
        return "up_to_date";
    case State::Available:
        return "available";
    case State::Installing:
        return "installing";
    case State::Restarting:
        return "restarting";
    case State::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace gateway::update
