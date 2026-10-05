#include "WebServer.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Esp.h>
#include <GatewayStream.h>
#include <JoinRequest.h>
#include <RadioPowerControl.h>
#include <TelemetryFrames.h>
#include <UserManagement.h>
#include <ArduinoJson.h>
#include <nvs.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>

#include <atomic>
#include <memory>
#include <new>

#include "ApiVersion.h"
#include "AuthenticationService.h"
#include "BackupService.h"
#include "BoardProfile.h"
#include "CommandService.h"
#include "CommissioningService.h"
#include "ConfigurationStore.h"
#include "Diagnostics.h"
#include "DeviceIdentity.h"
#include "EthernetService.h"
#include "FirmwareVersion.h"
#include "GatewayStatus.h"
#include "NodeRegistryStore.h"
#include "OtaService.h"
#include "PasswordHashService.h"
#include "PowerControlService.h"
#include "RadioConfig.h"
#include "RadioService.h"
#include "RecoveryService.h"
#include "TelemetryStore.h"
#include "TimeService.h"
#include "UpdateService.h"
#include "WebUiService.h"

namespace gateway::web_server {
namespace {

AsyncWebServer server(80);
AsyncWebSocket telemetrySocket("/ws");

constexpr uint16_t kWebSocketKeepAliveSeconds = 30;
constexpr uint16_t kMaximumWebSocketClients = 4;

uint32_t uptimeSeconds() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}

// A stream client names itself with the `X-Client` handshake header. That is
// the only honest source for the name: an API key's name is free text that
// somebody typed and the gateway accepts duplicates of it, so it says nothing
// about who is on the socket. A client that sends no header stays unnamed
// rather than being guessed at.
//
// Slot id 0 means free; AsyncWebSocket hands out ids from 1. Slots are keyed by
// client id and confirmed against the socket when read, so a missed disconnect
// leaks a slot but never a wrong name.
constexpr size_t kStreamClientNameSize = 32;

struct StreamClientIdentity {
    uint32_t id;
    char name[kStreamClientNameSize + 1];
};

StreamClientIdentity streamClients[kMaximumWebSocketClients]{};

std::atomic<uint32_t> websocketConnections{0};
std::atomic<uint32_t> websocketMessagesSent{0};
std::atomic<uint32_t> websocketMessagesDropped{0};
SemaphoreHandle_t setupMutex = nullptr;
SemaphoreHandle_t managementMutex = nullptr;
constexpr const char* kSessionCookieName = "rs_session";

void sendError(AsyncWebServerRequest* request, int status, const char* code);

bool hashPassword(
    const char* password, const size_t passwordLength,
    radiosensors::user_management::PasswordCredential& credential) {
    if (password == nullptr || passwordLength < 8 || passwordLength > 128)
        return false;
    credential.algorithm =
        radiosensors::gateway_storage::PasswordHashAlgorithm::Pbkdf2HmacSha256;
    credential.iterations = password_hash::kDefaultIterations;
    esp_fill_random(credential.salt, sizeof(credential.salt));
    return password_hash::computePbkdf2Sha256(
        password, passwordLength,
        credential.salt, sizeof(credential.salt), credential.iterations,
        credential.hash, sizeof(credential.hash));
}

const char* roleName(const radiosensors::gateway_storage::UserRole role) {
    return role == radiosensors::gateway_storage::UserRole::Admin
        ? "admin" : "viewer";
}

bool readSessionToken(
    AsyncWebServerRequest* request,
    char output[authentication::kSessionTokenCharacters + 1]) {
    if (!request->hasHeader("Cookie")) return false;
    const String cookies = request->getHeader("Cookie")->value();
    const String prefix = String(kSessionCookieName) + "=";
    int position = 0;
    while ((position = cookies.indexOf(prefix, position)) >= 0) {
        if (position == 0 || cookies[position - 1] == ';' ||
            cookies[position - 1] == ' ') {
            const int valueStart = position + prefix.length();
            int valueEnd = cookies.indexOf(';', valueStart);
            if (valueEnd < 0) valueEnd = cookies.length();
            if (valueEnd - valueStart ==
                static_cast<int>(authentication::kSessionTokenCharacters)) {
                memcpy(
                    output, cookies.c_str() + valueStart,
                    authentication::kSessionTokenCharacters);
                output[authentication::kSessionTokenCharacters] = '\0';
                return true;
            }
        }
        position += prefix.length();
    }
    return false;
}

void addSessionCookie(
    AsyncWebServerResponse* response, const char* sessionToken) {
    char value[160]{};
    snprintf(
        value, sizeof(value),
        "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200",
        kSessionCookieName, sessionToken);
    response->addHeader("Set-Cookie", value);
}

void clearSessionCookie(AsyncWebServerResponse* response) {
    response->addHeader(
        "Set-Cookie",
        "rs_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
}

void sendPrincipal(
    AsyncWebServerRequest* request,
    const authentication::Principal& principal,
    const char* sessionToken = nullptr,
    const int statusCode = 200) {
    char body[256]{};
    snprintf(
        body, sizeof(body),
        "{\"user\":{\"id\":%lu,\"username\":\"%s\",\"role\":\"%s\"},"
        "\"csrf_token\":\"%s\"}",
        static_cast<unsigned long>(principal.userId), principal.username,
        roleName(principal.role), principal.csrfToken);
    AsyncWebServerResponse* response =
        request->beginResponse(statusCode, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    if (sessionToken != nullptr) addSessionCookie(response, sessionToken);
    request->send(response);
}

bool authorizeSession(
    AsyncWebServerRequest* request, authentication::Principal& principal,
    const bool requireCsrf = false) {
    if (recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return false;
    }
    char sessionToken[authentication::kSessionTokenCharacters + 1]{};
    if (!readSessionToken(request, sessionToken) ||
        !authentication::authorize(
            sessionToken, nullptr, false, principal)) {
        sendError(request, 401, "authentication_required");
        return false;
    }
    if (!requireCsrf) return true;
    String csrfHeader;
    const char* csrfToken = nullptr;
    if (request->hasHeader("X-CSRF-Token")) {
        csrfHeader = request->getHeader("X-CSRF-Token")->value();
        csrfToken = csrfHeader.c_str();
    }
    if (!authentication::authorize(
            sessionToken, csrfToken, true, principal)) {
        sendError(request, 403, "invalid_csrf_token");
        return false;
    }
    return true;
}

bool authorizeAdmin(
    AsyncWebServerRequest* request, authentication::Principal& principal,
    const bool requireCsrf) {
    if (!authorizeSession(request, principal, requireCsrf)) return false;
    if (principal.role != radiosensors::gateway_storage::UserRole::Admin) {
        sendError(request, 403, "admin_required");
        return false;
    }
    return true;
}

// Without storage the gateway id falls back to one derived from the MAC, and
// the tokens are not loaded. Answering 503 keeps an external client from
// taking that id for another gateway or the rejected token for a revoked one.
bool rejectWithoutStorage(AsyncWebServerRequest* request) {
    if (configuration_store::ready()) return false;
    sendError(request, 503,
              recovery::blocked() ? recovery::reason() : "storage_unavailable");
    return true;
}

// The registry and the stream share one scope: a consumer needs both to be of
// any use, so gating them separately only produced tokens that authenticate
// here and fail at /ws, or the reverse.
bool authorizeRegistryRead(AsyncWebServerRequest* request) {
    if (rejectWithoutStorage(request)) return false;
    char sessionToken[authentication::kSessionTokenCharacters + 1]{};
    authentication::Principal principal{};
    if (readSessionToken(request, sessionToken) &&
        authentication::authorize(sessionToken, nullptr, false, principal)) {
        return true;
    }
    if (request->hasHeader("Authorization")) {
        const String header = request->getHeader("Authorization")->value();
        if (header.startsWith("Bearer ") &&
            authentication::authorizeBearer(
                header.c_str() + 7,
                radiosensors::gateway_storage::TokenScope::TelemetryRead)) {
            return true;
        }
    }
    sendError(request, 401, "authentication_required");
    return false;
}

bool authorizeTelemetryStream(AsyncWebServerRequest* request) {
    char sessionToken[authentication::kSessionTokenCharacters + 1]{};
    authentication::Principal principal{};
    if (readSessionToken(request, sessionToken) &&
        authentication::authorize(sessionToken, nullptr, false, principal)) {
        return true;
    }
    if (!request->hasHeader("Authorization")) return false;
    const String header = request->getHeader("Authorization")->value();
    return header.startsWith("Bearer ") &&
        authentication::authorizeBearer(
            header.c_str() + 7,
            radiosensors::gateway_storage::TokenScope::TelemetryRead);
}

void sendError(AsyncWebServerRequest* request, int status, const char* code) {
    char body[96]{};
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    request->send(status, "application/json", body);
}

void handleSetupStatus(AsyncWebServerRequest* request) {
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    response->printf(
        "{\"setup_required\":%s,\"physical_window_active\":%s,"
        "\"remaining_seconds\":%lu,\"recovery_required\":%s,"
        "\"recovery_reason\":\"%s\"}",
        status::setupRequired() ? "true" : "false",
        status::setupActive() ? "true" : "false",
        static_cast<unsigned long>(status::setupRemainingSeconds()),
        recovery::blocked() || !configuration_store::ready() ? "true" : "false",
        !configuration_store::ready() && !recovery::blocked()
            ? "storage_unavailable"
            : recovery::reason());
    request->send(response);
}

constexpr uint32_t kRestartDelayMs = 500;
// A completed setup or radio reset restarts after its response leaves the socket.
uint32_t restartAtMs = 0;

uint8_t randomOperationalNetworkId() {
    uint8_t value = 0;
    do value = static_cast<uint8_t>(esp_random());
    while (value == 0);
    return value;
}

void handleInitialSetup(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    if (setupMutex == nullptr || xSemaphoreTake(setupMutex, 0) != pdTRUE) {
        sendError(request, 409, "setup_busy");
        return;
    }
    if (!status::setupRequired()) {
        xSemaphoreGive(setupMutex);
        sendError(request, 409, "setup_already_complete");
        return;
    }
    if (!status::setupActive()) {
        xSemaphoreGive(setupMutex);
        sendError(request, 403, "physical_setup_required");
        return;
    }
    if (!json.is<JsonObject>()) {
        xSemaphoreGive(setupMutex);
        sendError(request, 400, "invalid_request");
        return;
    }
    JsonObject object = json.as<JsonObject>();
    const char* username = object["username"].is<const char*>()
        ? object["username"].as<const char*>() : nullptr;
    const char* password = object["password"].is<const char*>()
        ? object["password"].as<const char*>() : nullptr;
    const char* hostname = object["hostname"].is<const char*>()
        ? object["hostname"].as<const char*>() : "";
    const size_t usernameLength = username == nullptr ? 0 : strlen(username);
    const size_t passwordLength = password == nullptr ? 0 : strlen(password);
    const size_t hostnameLength = strlen(hostname);
    if (!radiosensors::user_management::validUsername(username, usernameLength) ||
        passwordLength < 8 || passwordLength > 128 ||
        hostnameLength > radiosensors::gateway_storage::kHostnameSize) {
        xSemaphoreGive(setupMutex);
        sendError(request, 422, "invalid_setup_values");
        return;
    }

    auto secrets = configuration_store::secrets();
    secrets.radioConfigured = true;
    if (object["operational_network_id"].is<uint8_t>()) {
        const uint8_t requested = object["operational_network_id"].as<uint8_t>();
        if (requested == 0) {
            xSemaphoreGive(setupMutex);
            sendError(request, 422, "invalid_operational_network_id");
            return;
        }
        secrets.operationalNetworkId = requested;
    }

    auto settings = configuration_store::settings();
    memset(settings.hostname, 0, sizeof(settings.hostname));
    settings.hostnameLength = static_cast<uint8_t>(hostnameLength);
    memcpy(settings.hostname, hostname, hostnameLength);

    auto authentication = radiosensors::gateway_storage::defaultAuthentication();
    authentication.userCount = 1;
    authentication.nextUserId = 2;
    auto& user = authentication.users[0];
    user.id = 1;
    user.usernameLength = static_cast<uint8_t>(usernameLength);
    memcpy(user.username, username, usernameLength);
    user.role = radiosensors::gateway_storage::UserRole::Admin;
    user.enabled = true;
    user.hashAlgorithm =
        radiosensors::gateway_storage::PasswordHashAlgorithm::Pbkdf2HmacSha256;
    user.pbkdf2Iterations = password_hash::kDefaultIterations;
    esp_fill_random(user.salt, sizeof(user.salt));
    const uint32_t hashStartedAt = millis();
    const bool hashResult = password_hash::computePbkdf2Sha256(
        password, passwordLength,
        user.salt, sizeof(user.salt), user.pbkdf2Iterations,
        user.passwordHash, sizeof(user.passwordHash));
    if (!hashResult) {
        xSemaphoreGive(setupMutex);
        sendError(request, 500, "password_hash_failed");
        return;
    }
    Serial.printf("Initial password hash completed in %lu ms (%lu iterations)\n",
                  static_cast<unsigned long>(millis() - hashStartedAt),
                  static_cast<unsigned long>(user.pbkdf2Iterations));

    const auto secretResult = configuration_store::saveSecrets(secrets);
    if ((secretResult != configuration_store::SaveStatus::Ok &&
         secretResult != configuration_store::SaveStatus::NoChange)) {
        xSemaphoreGive(setupMutex);
        sendError(request, 500, "setup_storage_failed");
        return;
    }
    const auto settingsResult = configuration_store::saveSettings(settings);
    if (settingsResult != configuration_store::SaveStatus::Ok &&
        settingsResult != configuration_store::SaveStatus::NoChange) {
        xSemaphoreGive(setupMutex);
        sendError(request, 500, "setup_storage_failed");
        return;
    }
    if (configuration_store::saveAuthentication(authentication) !=
        configuration_store::SaveStatus::Ok) {
        xSemaphoreGive(setupMutex);
        sendError(request, 500, "setup_storage_failed");
        return;
    }
    // Initial setup enables the operational network after its settings commit.
    if (!radio::applyInstallation(
            secrets.operationalNetworkId)) {
        Serial.println("Initial setup: radio did not take the operational network");
    }

    status::closeSetup();
    xSemaphoreGive(setupMutex);
    authentication::LoginResult login{};
    if (authentication::createSession(
            user.id, user.role, user.username, user.usernameLength, login) ==
        authentication::LoginStatus::Ok) {
        sendPrincipal(request, login.principal, login.sessionToken, 201);
    } else {
        request->send(201, "application/json", "{\"status\":\"configured\"}");
    }
    if (hostnameLength != 0) restartAtMs = millis() + kRestartDelayMs;
}

// Wipes the passwords from the parsed document and the raw body it came from.
struct BackupRequestSecrets {
    AsyncWebServerRequest* request;
    JsonVariant& json;

    ~BackupRequestSecrets() {
        for (const char* field : {"password", "admin_password"}) {
            const JsonString value = json[field].as<JsonString>();
            if (value.c_str() != nullptr) {
                backup::wipe(const_cast<char*>(value.c_str()), value.size());
            }
        }
        // AsyncJson owns this allocation until the request is destroyed.
        if (request->_tempObject != nullptr) {
            backup::wipe(request->_tempObject, request->contentLength());
        }
    }
};

void handleBackupExport(AsyncWebServerRequest* request, JsonVariant& json) {
    BackupRequestSecrets sensitive{request, json};
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    backup::Operation operation;
    if (!operation || recovery::blocked() || status::pairingActive()) {
        sendError(request, 409, "backup_busy");
        return;
    }
    const JsonString password = json["password"].as<JsonString>();
    if (!backup::validPassword(password.c_str(), password.size())) {
        sendError(request, 422, "invalid_backup_password");
        return;
    }
    std::unique_ptr<backup::Snapshot> snapshot(new (std::nothrow) backup::Snapshot);
    if (!snapshot) {
        sendError(request, 503, "backup_failed");
        return;
    }
    if (!backup::capture(*snapshot)) {
        sendError(request, 409, "backup_busy");
        return;
    }
    const uint32_t startedAt = millis();
    std::string file;
    if (!backup::encrypt(*snapshot, password.c_str(), password.size(), file)) {
        Serial.println("Backup export failed: encryption error");
        sendError(request, 500, "backup_failed");
        return;
    }
    Serial.printf(
        "Backup exported: %u nodes, %u bytes in %lu ms\n",
        static_cast<unsigned>(snapshot->nodes.size()),
        static_cast<unsigned>(file.size()),
        static_cast<unsigned long>(millis() - startedAt));
    AsyncResponseStream* response =
        request->beginResponseStream("application/octet-stream");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader(
        "Content-Disposition", "attachment; filename=osk-sense.oskbackup");
    response->write(reinterpret_cast<const uint8_t*>(file.data()), file.size());
    request->send(response);
}

void sendBackupPreview(
    AsyncWebServerRequest* request, const backup::Snapshot& snapshot) {
    char gatewayId[33];
    backup::gatewayId(snapshot, gatewayId);
    JsonDocument document;
    document["gateway_id"] = gatewayId;
    document["created_at_ms"] = snapshot.createdAt;
    document["node_count"] = snapshot.nodes.size();
    document["hostname"] =
        std::string(snapshot.settings.hostname, snapshot.settings.hostnameLength);
    document["mdns_enabled"] = snapshot.settings.mdnsEnabled;
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

// Base64 file, both passwords at their worst JSON escaping (\u00XX per byte),
// the username, and the keys.
constexpr size_t kMaxBackupImportBody =
    4 * ((backup::kMaxFile + 2) / 3) + 2 * 6 * 128 + 32 + 64;

// Serves both /ui/backup/preview and /ui/backup/restore: each decrypts and
// validates the uploaded file itself rather than trusting an earlier preview.
// Only restore needs the physical window: preview writes nothing and reveals
// nothing that the holder of the file and its password could not decrypt.
void handleBackupImport(AsyncWebServerRequest* request, JsonVariant& json) {
    BackupRequestSecrets sensitive{request, json};
    const bool preview = request->url().endsWith("/preview");
    backup::Operation operation;
    if (!operation) {
        sendError(request, 409, "backup_busy");
        return;
    }
    recovery::Guard guard(0);
    if (!guard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    if (!backup::cleanGateway()) {
        sendError(request, 409, "restore_requires_clean_gateway");
        return;
    }
    if (!preview && !status::setupActive()) {
        sendError(request, 403, "physical_setup_required");
        return;
    }
    // A custom header keeps cross-origin forms away from this unauthenticated endpoint.
    if (!request->hasHeader("X-Backup-Request") ||
        request->getHeader("X-Backup-Request")->value() != "1") {
        sendError(request, 403, "invalid_request");
        return;
    }
    const JsonString password = json["password"].as<JsonString>();
    const JsonString encoded = json["file"].as<JsonString>();
    if (!backup::validPassword(password.c_str(), password.size()) ||
        encoded.size() == 0 || encoded.size() > 4 * ((backup::kMaxFile + 2) / 3)) {
        sendError(request, 422, "invalid_backup");
        return;
    }
    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[backup::kMaxFile]);
    std::unique_ptr<backup::Snapshot> snapshot(new (std::nothrow) backup::Snapshot);
    if (!bytes || !snapshot) {
        sendError(request, 503, "backup_failed");
        return;
    }
    const uint32_t startedAt = millis();
    size_t length = 0;
    if (mbedtls_base64_decode(
            bytes.get(), backup::kMaxFile, &length,
            reinterpret_cast<const uint8_t*>(encoded.c_str()), encoded.size()) != 0 ||
        !backup::decrypt(
            bytes.get(), length, password.c_str(), password.size(), *snapshot)) {
        Serial.println("Backup import rejected: wrong password or invalid file");
        sendError(request, 422, "invalid_backup");
        return;
    }
    Serial.printf(
        "Backup decrypted: %u nodes in %lu ms\n",
        static_cast<unsigned>(snapshot->nodes.size()),
        static_cast<unsigned long>(millis() - startedAt));
    if (preview) {
        sendBackupPreview(request, *snapshot);
        return;
    }
    // The key derivation takes seconds; the window may have closed meanwhile.
    if (!status::setupActive()) {
        sendError(request, 403, "physical_setup_required");
        return;
    }

    const JsonString username = json["username"].as<JsonString>();
    const JsonString adminPassword = json["admin_password"].as<JsonString>();
    if (!radiosensors::user_management::validUsername(username.c_str(), username.size()) ||
        adminPassword.size() < 8 || adminPassword.size() > 128 ||
        memchr(adminPassword.c_str(), 0, adminPassword.size()) != nullptr) {
        sendError(request, 422, "invalid_setup_values");
        return;
    }
    auto admin = radiosensors::gateway_storage::defaultAuthentication();
    admin.userCount = 1;
    admin.nextUserId = 2;
    auto& user = admin.users[0];
    user.id = 1;
    user.enabled = true;
    user.role = radiosensors::gateway_storage::UserRole::Admin;
    user.usernameLength = username.size();
    memcpy(user.username, username.c_str(), username.size());
    radiosensors::user_management::PasswordCredential credential{};
    if (!hashPassword(adminPassword.c_str(), adminPassword.size(), credential)) {
        sendError(request, 500, "password_hash_failed");
        return;
    }
    user.hashAlgorithm = credential.algorithm;
    user.pbkdf2Iterations = credential.iterations;
    memcpy(user.salt, credential.salt, sizeof(user.salt));
    memcpy(user.passwordHash, credential.hash, sizeof(user.passwordHash));
    backup::wipe(&credential, sizeof(credential));

    const bool restored = backup::restore(*snapshot, admin);
    backup::wipe(&admin, sizeof(admin));
    if (!restored) {
        sendError(request, 500, "restore_failed");
        return;
    }
    request->send(200, "application/json", "{\"status\":\"restarting\"}");
}

void handleLogin(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* username = object["username"].is<const char*>()
        ? object["username"].as<const char*>() : nullptr;
    const char* password = object["password"].is<const char*>()
        ? object["password"].as<const char*>() : nullptr;
    authentication::LoginResult result{};
    switch (authentication::login(username, password, result)) {
        case authentication::LoginStatus::Ok:
            sendPrincipal(request, result.principal, result.sessionToken);
            return;
        case authentication::LoginStatus::InvalidCredentials:
            sendError(request, 401, "invalid_credentials");
            return;
        case authentication::LoginStatus::Busy:
            sendError(request, 409, "login_busy");
            return;
        case authentication::LoginStatus::StorageUnavailable:
            sendError(request, 503, "storage_unavailable");
            return;
        case authentication::LoginStatus::NoSessionCapacity:
            sendError(request, 503, "session_unavailable");
            return;
    }
}

void handleCurrentSession(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal)) return;
    sendPrincipal(request, principal);
}

void handleLogout(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal, true)) return;
    char sessionToken[authentication::kSessionTokenCharacters + 1]{};
    readSessionToken(request, sessionToken);
    authentication::logout(sessionToken);
    AsyncWebServerResponse* response = request->beginResponse(204);
    clearSessionCookie(response);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

const char* nodeStateName(const radiosensors::registry::NodeState state) {
    switch (state) {
        case radiosensors::registry::NodeState::Pending: return "pending";
        case radiosensors::registry::NodeState::Active: return "active";
        case radiosensors::registry::NodeState::Disabled: return "disabled";
    }
    return "unknown";
}

void handleNodes(AsyncWebServerRequest* request) {
    if (!authorizeRegistryRead(request)) return;
    const std::unique_ptr<registry_store::Snapshot> registry(
        new (std::nothrow) registry_store::Snapshot());
    if (!registry || !registry_store::snapshot(*registry)) {
        sendError(request, 503, "registry_unavailable");
        return;
    }
    AsyncResponseStream* response =
        request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    response->printf(
        "{\"registry_generation\":%lu,\"nodes\":[",
        static_cast<unsigned long>(registry->generation));
    for (size_t index = 0; index < registry->count; ++index) {
        const auto& node = registry->records[index];
        if (index != 0) response->print(',');
        JsonDocument document;
        document["node_id"] = node.nodeId;
        char uid[radiosensors::protocol::kDeviceUidSize * 2 + 1]{};
        for (size_t uidIndex = 0; uidIndex < sizeof(node.deviceUid); ++uidIndex)
            snprintf(uid + uidIndex * 2, 3, "%02X", node.deviceUid[uidIndex]);
        document["device_uid"] = uid;
        document["display_name"] = String(
            node.displayName, node.displayNameLength);
        document["profile_id"] = node.profileId;
        char firmware[16]{};
        snprintf(firmware, sizeof(firmware), "%u.%u.%u", node.firmware.major,
                 node.firmware.minor, node.firmware.patch);
        document["firmware"] = firmware;
        document["state"] = nodeStateName(node.state);
        document["max_power_level"] = node.maxPowerLevel;
        const bool fixedPower =
            radiosensors::radio_power::isFixedPolicy(node.powerPolicy);
        document["power_policy"] = fixedPower ? "fixed" : "auto";
        if (fixedPower) {
            document["fixed_power_level"] =
                radiosensors::radio_power::fixedLevel(node.powerPolicy);
        }
        const uint8_t powerTarget = power_control::desiredLevel(node.nodeId);
        if (powerTarget != power_control::kNoTarget) {
            document["tx_power_target"] = powerTarget;
        }
        telemetry_store::Record telemetry{};
        if (telemetry_store::find(node.nodeId, telemetry)) {
            document["last_seen_at_ms"] = telemetry.receivedAtUnixMs;
            document["rssi"] = telemetry.rssi;
            document["has_telemetry"] = true;
            radiosensors::protocol::TelemetryView view{};
            if (radiosensors::protocol::decodeTelemetry(
                    telemetry.data, telemetry.size, view) ==
                radiosensors::protocol::TelemetryCodecStatus::Ok) {
                document["tx_power_level"] =
                    radiosensors::protocol::radioPowerLevel(view.radioState);
                document["radio_fallback"] =
                    (view.radioState & radiosensors::protocol::kRadioFallback) != 0;
                if (view.downlinkRssi != radiosensors::protocol::kNoDownlinkRssi) {
                    document["downlink_rssi"] = view.downlinkRssi;
                }
                if (view.supplyMillivolts !=
                    radiosensors::protocol::kInvalidSupplyVoltage) {
                    document["supply_mv"] = view.supplyMillivolts;
                }
            }
        } else {
            document["has_telemetry"] = false;
        }
        serializeJson(document, *response);
    }
    response->print("]}");
    request->send(response);
}

// Reads `power_policy` and `fixed_power_level` into the registry encoding.
bool parsePowerPolicy(const JsonObjectConst object, uint8_t& policy) {
    const char* const mode = object["power_policy"].as<const char*>();
    if (mode == nullptr) return false;
    if (strcmp(mode, "auto") == 0) {
        policy = radiosensors::radio_power::kPolicyAuto;
        return true;
    }
    if (strcmp(mode, "fixed") != 0 || !object["fixed_power_level"].is<uint8_t>()) {
        return false;
    }
    const uint8_t level = object["fixed_power_level"].as<uint8_t>();
    if (level > radiosensors::protocol::kMaxRadioPowerLevel) return false;
    policy = radiosensors::radio_power::fixedPolicy(level);
    return true;
}

bool sendRegistryCommitError(
    AsyncWebServerRequest* request, const RegistryCommitStatus commit) {
    if (commit == RegistryCommitStatus::StorageError) {
        sendError(request, 500, "node_storage_failed");
    } else if (commit == RegistryCommitStatus::NotInitialized) {
        sendError(request, 503, "registry_unavailable");
    } else {
        return false;
    }
    return true;
}

// Renames a node, changes its radio power policy, or both.
void handlePatchNode(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObjectConst object = json.as<JsonObjectConst>();
    const bool hasName = !object["display_name"].isNull();
    const bool hasPolicy = !object["power_policy"].isNull();
    if (!object["node_id"].is<uint8_t>() || (!hasName && !hasPolicy) ||
        (hasName && !object["display_name"].is<const char*>())) {
        sendError(request, 422, "invalid_node_values");
        return;
    }
    uint8_t policy = radiosensors::radio_power::kPolicyAuto;
    if (hasPolicy && !parsePowerPolicy(object, policy)) {
        sendError(request, 422, "invalid_power_policy");
        return;
    }
    const uint8_t nodeId = object["node_id"].as<uint8_t>();

    if (hasName) {
        const char* const displayName = object["display_name"].as<const char*>();
        radiosensors::registry::RenameStatus renameStatus{};
        const RegistryCommitStatus commit = registry_store::renameAndSave(
            nodeId, displayName, strlen(displayName), renameStatus);
        if (renameStatus == radiosensors::registry::RenameStatus::InvalidName) {
            sendError(request, 422, "invalid_display_name");
            return;
        }
        if (renameStatus == radiosensors::registry::RenameStatus::NotFound) {
            sendError(request, 404, "node_not_found");
            return;
        }
        if (sendRegistryCommitError(request, commit)) return;
    }
    if (hasPolicy) {
        radiosensors::registry::PowerPolicyStatus policyStatus{};
        const RegistryCommitStatus commit =
            registry_store::setPowerPolicyAndSave(nodeId, policy, policyStatus);
        if (policyStatus == radiosensors::registry::PowerPolicyStatus::InvalidPolicy) {
            sendError(request, 422, "invalid_power_policy");
            return;
        }
        if (policyStatus == radiosensors::registry::PowerPolicyStatus::NotFound) {
            sendError(request, 404, "node_not_found");
            return;
        }
        if (sendRegistryCommitError(request, commit)) return;
        uint8_t ceiling = 0;
        uint8_t storedPolicy = 0;
        if (registry_store::radioPolicy(nodeId, ceiling, storedPolicy)) {
            power_control::policyChanged(nodeId, storedPolicy, ceiling);
        }
    }

    JsonDocument response;
    response["node_id"] = nodeId;
    if (hasName) response["display_name"] = object["display_name"].as<const char*>();
    if (hasPolicy) {
        const bool fixedPower = radiosensors::radio_power::isFixedPolicy(policy);
        response["power_policy"] = fixedPower ? "fixed" : "auto";
        if (fixedPower) {
            response["fixed_power_level"] = radiosensors::radio_power::fixedLevel(policy);
        }
    }
    response["registry_generation"] = registry_store::generation();
    String body;
    serializeJson(response, body);
    request->send(200, "application/json", body);
}

void handleDeleteNode(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() ||
        !json.as<JsonObjectConst>()["node_id"].is<uint8_t>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const uint8_t nodeId = json.as<JsonObjectConst>()["node_id"].as<uint8_t>();
    bool removed = false;
    const RegistryCommitStatus commit =
        registry_store::removeAndSave(nodeId, removed);
    if (commit == RegistryCommitStatus::StorageError) {
        sendError(request, 500, "node_storage_failed");
    } else if (commit == RegistryCommitStatus::NotInitialized) {
        sendError(request, 503, "registry_unavailable");
    } else if (!removed) {
        sendError(request, 404, "node_not_found");
    } else {
        telemetry_store::erase(nodeId);
        // Best effort: a record left behind is bound to the deleted node's UID
        // and is never delivered to a node that reuses the ID.
        commands::removeNode(nodeId);
        power_control::forget(nodeId);
        request->send(204);
    }
}

using radiosensors::protocol::CommandStatus;
using radiosensors::protocol::CommandType;

const char* commandTypeName(const uint8_t type) {
    switch (static_cast<CommandType>(type)) {
        case CommandType::SetCount:
            return "set_count";
        case CommandType::ReadInfo:
            return "read_info";
    }
    return "unknown";
}

bool parseCommandType(const char* const name, CommandType& type) {
    if (strcmp(name, "set_count") == 0) {
        type = CommandType::SetCount;
        return true;
    }
    if (strcmp(name, "read_info") == 0) {
        type = CommandType::ReadInfo;
        return true;
    }
    return false;
}

const char* commandStatusName(const CommandStatus status) {
    switch (status) {
        case CommandStatus::Applied:
            return "applied";
        case CommandStatus::Unsupported:
            return "unsupported";
        case CommandStatus::InvalidArgument:
            return "invalid_argument";
        case CommandStatus::StorageFailure:
            return "storage_failure";
    }
    return "unknown";
}

void writeCommand(JsonObject object, const commands::Entry& entry) {
    const radiosensors::gateway_storage::CommandRecord& record = entry.record;
    const CommandType type = static_cast<CommandType>(record.type);
    object["node_id"] = record.nodeId;
    object["command_id"] = record.commandId;
    object["type"] = commandTypeName(record.type);
    JsonObject arguments = object["arguments"].to<JsonObject>();
    if (type == CommandType::SetCount) {
        arguments["count"] = radiosensors::protocol::readUint32Le(record.arguments);
    }
    object["queued_at_ms"] = record.queuedAtUnixMs;
    if (record.state == radiosensors::gateway_storage::CommandState::Pending) {
        object["state"] = entry.delivered ? "delivered" : "pending";
        return;
    }
    object["state"] = "completed";
    object["status"] = commandStatusName(record.status);
    object["completed_at_ms"] = record.completedAtUnixMs;
    if (record.status != CommandStatus::Applied) return;
    if (type == CommandType::SetCount) {
        JsonObject result = object["result"].to<JsonObject>();
        result["previous_count"] = radiosensors::protocol::readUint32Le(record.result);
        result["count"] = radiosensors::protocol::readUint32Le(record.result + 4);
    }
    radiosensors::protocol::NodeInfo info{};
    if (type == CommandType::ReadInfo &&
        radiosensors::protocol::decodeNodeInfo(record.result, record.resultSize, info)) {
        JsonObject result = object["result"].to<JsonObject>();
        result["profile_id"] = info.profileId;
        char firmware[16]{};
        snprintf(firmware, sizeof(firmware), "%u.%u.%u", info.firmware.major,
                 info.firmware.minor, info.firmware.patch);
        result["firmware"] = firmware;
        result["max_power_level"] = info.maxPowerLevel;
    }
}

void handleCommands(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal, false)) return;
    const std::unique_ptr<commands::Listing> listing(
        new (std::nothrow) commands::Listing());
    if (!listing || !commands::list(*listing)) {
        sendError(request, 503, "commands_unavailable");
        return;
    }
    JsonDocument document;
    JsonArray array = document["commands"].to<JsonArray>();
    for (size_t index = 0; index < listing->count; ++index) {
        writeCommand(array.add<JsonObject>(), listing->entries[index]);
    }
    String body;
    serializeJson(document, body);
    AsyncWebServerResponse* response =
        request->beginResponse(200, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void handleQueueCommand(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObjectConst object = json.as<JsonObjectConst>();
    if (!object["node_id"].is<uint8_t>() || !object["type"].is<const char*>() ||
        !object["arguments"].is<JsonObjectConst>()) {
        sendError(request, 422, "invalid_command_values");
        return;
    }
    CommandType type{};
    if (!parseCommandType(object["type"].as<const char*>(), type)) {
        sendError(request, 422, "unsupported_command");
        return;
    }
    const JsonObjectConst arguments = object["arguments"].as<JsonObjectConst>();
    uint8_t encoded[radiosensors::protocol::kMaxCommandArgumentSize]{};
    size_t size = 0;
    if (type == CommandType::SetCount && arguments["count"].is<uint32_t>()) {
        radiosensors::protocol::writeUint32Le(encoded, arguments["count"].as<uint32_t>());
        size = radiosensors::protocol::kSetCountArgumentSize;
    } else if (type != CommandType::ReadInfo || arguments.size() != 0) {
        sendError(request, 422, "invalid_command_arguments");
        return;
    }

    commands::Entry queued{};
    switch (commands::queue(object["node_id"].as<uint8_t>(), type, encoded, size, queued)) {
        case commands::QueueResult::Queued: {
            JsonDocument document;
            writeCommand(document.to<JsonObject>(), queued);
            String body;
            serializeJson(document, body);
            request->send(201, "application/json", body);
            return;
        }
        case commands::QueueResult::NodeNotFound:
            sendError(request, 404, "node_not_found");
            return;
        case commands::QueueResult::Unsupported:
            sendError(request, 422, "unsupported_command");
            return;
        case commands::QueueResult::InvalidArguments:
            sendError(request, 422, "invalid_command_arguments");
            return;
        case commands::QueueResult::Busy:
            sendError(request, 409, "command_pending");
            return;
        case commands::QueueResult::CapacityReached:
            sendError(request, 409, "command_capacity_reached");
            return;
        case commands::QueueResult::StorageError:
            sendError(request, 500, "command_storage_failed");
            return;
        case commands::QueueResult::NotInitialized:
            sendError(request, 503, "commands_unavailable");
            return;
    }
}

void handleCancelCommand(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() ||
        !json.as<JsonObjectConst>()["node_id"].is<uint8_t>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    switch (commands::cancel(json.as<JsonObjectConst>()["node_id"].as<uint8_t>())) {
        case commands::CancelResult::Cancelled:
            request->send(204);
            return;
        case commands::CancelResult::NotFound:
            sendError(request, 404, "command_not_found");
            return;
        case commands::CancelResult::StorageError:
            sendError(request, 500, "command_storage_failed");
            return;
        case commands::CancelResult::NotInitialized:
            sendError(request, 503, "commands_unavailable");
            return;
    }
}

void handleResetRadioNetwork(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;

    // The body is optional: without one the gateway picks the network id.
    uint8_t requestedNetworkId = 0;
    if (json.is<JsonObject>()) {
        const JsonObjectConst object = json.as<JsonObjectConst>();
        if (!object["operational_network_id"].isNull()) {
            if (!object["operational_network_id"].is<uint8_t>() ||
                object["operational_network_id"].as<uint8_t>() == 0) {
                sendError(request, 422, "invalid_operational_network_id");
                return;
            }
            requestedNetworkId = object["operational_network_id"].as<uint8_t>();
        }
    }

    // Clear the registry first: the nodes in it are bound to the old network
    // and key, and saveSecrets refuses to change the radio profile while any
    // of them is still active.
    size_t removedNodes = 0;
    const RegistryCommitStatus registryStatus = registry_store::clearAndSave(removedNodes);
    if (registryStatus == RegistryCommitStatus::StorageError) {
        sendError(request, 500, "node_storage_failed");
        return;
    }
    if (registryStatus == RegistryCommitStatus::NotInitialized) {
        sendError(request, 503, "registry_unavailable");
        return;
    }
    telemetry_store::clear();
    commands::clear();
    power_control::clear();

    auto secrets = configuration_store::secrets();
    secrets.radioConfigured = true;
    secrets.operationalNetworkId = requestedNetworkId != 0
        ? requestedNetworkId
        : randomOperationalNetworkId();

    const auto secretResult = configuration_store::saveSecrets(secrets);
    if (secretResult == configuration_store::SaveStatus::LockedByActiveNodes) {
        sendError(request, 409, "secrets_locked_by_active_nodes");
        return;
    }
    if (secretResult != configuration_store::SaveStatus::Ok) {
        sendError(request, 500, "secrets_storage_failed");
        return;
    }

    JsonDocument response;
    response["operational_network_id"] = secrets.operationalNetworkId;
    response["removed_nodes"] = removedNodes;
    response["restarting"] = true;
    String body;
    serializeJson(response, body);
    request->send(202, "application/json", body);
    restartAtMs = millis() + kRestartDelayMs;
}

// `scopes` is optional. Omitting it grants the only scope there is, which is
// what every caller wants and what the Web UI now sends. A field that is
// present is still validated: a client asking for a scope this firmware does
// not have is told so rather than quietly handed the one it does have.
bool parseTokenScopes(const JsonVariantConst& json, uint16_t& scopes) {
    scopes = radiosensors::gateway_storage::TokenScope::TelemetryRead;
    if (json.isNull()) return true;
    scopes = 0;
    if (!json.is<JsonArrayConst>()) return false;
    for (const JsonVariantConst item : json.as<JsonArrayConst>()) {
        if (!item.is<const char*>()) return false;
        const char* value = item.as<const char*>();
        if (strcmp(value, "telemetry:read") != 0) return false;
        scopes |= radiosensors::gateway_storage::TokenScope::TelemetryRead;
    }
    return scopes != 0;
}

void addTokenScopes(JsonArray output, const uint16_t scopes) {
    if ((scopes & radiosensors::gateway_storage::TokenScope::TelemetryRead) != 0)
        output.add("telemetry:read");
}

void addUser(JsonObject output,
             const radiosensors::gateway_storage::UserRecord& source) {
    output["id"] = source.id;
    output["username"] = String(source.username, source.usernameLength);
    output["role"] = roleName(source.role);
    output["enabled"] = source.enabled;
}

bool parseUserRole(const JsonVariantConst value,
                   radiosensors::gateway_storage::UserRole& role) {
    if (!value.is<const char*>()) return false;
    const char* name = value.as<const char*>();
    if (strcmp(name, "admin") == 0) {
        role = radiosensors::gateway_storage::UserRole::Admin;
        return true;
    }
    if (strcmp(name, "viewer") == 0) {
        role = radiosensors::gateway_storage::UserRole::Viewer;
        return true;
    }
    return false;
}

void sendUserMutationError(
    AsyncWebServerRequest* request,
    const radiosensors::user_management::Status status) {
    using radiosensors::user_management::Status;
    switch (status) {
        case Status::InvalidValue:
            sendError(request, 422, "invalid_user_values");
            break;
        case Status::NotFound:
            sendError(request, 404, "user_not_found");
            break;
        case Status::UsernameExists:
            sendError(request, 409, "username_already_exists");
            break;
        case Status::CapacityReached:
            sendError(request, 409, "user_capacity_reached");
            break;
        case Status::LastAdminRequired:
            sendError(request, 409, "last_admin_required");
            break;
        case Status::Ok:
            break;
    }
}

void handleUsers(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, false)) return;
    const auto data = configuration_store::authentication();
    JsonDocument document;
    JsonArray users = document["users"].to<JsonArray>();
    for (uint8_t index = 0; index < data.userCount; ++index)
        addUser(users.add<JsonObject>(), data.users[index]);
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

void handleCreateUser(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* username = object["username"].is<const char*>()
        ? object["username"].as<const char*>() : nullptr;
    const char* password = object["password"].is<const char*>()
        ? object["password"].as<const char*>() : nullptr;
    const size_t usernameLength = username == nullptr ? 0 : strlen(username);
    const size_t passwordLength = password == nullptr ? 0 : strlen(password);
    radiosensors::gateway_storage::UserRole role{};
    if (!radiosensors::user_management::validUsername(username, usernameLength) ||
        passwordLength < 8 ||
        passwordLength > 128 || !parseUserRole(object["role"], role) ||
        (!object["enabled"].isNull() && !object["enabled"].is<bool>())) {
        sendError(request, 422, "invalid_user_values");
        return;
    }
    if (managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, 409, "mutation_busy");
        return;
    }
    auto data = configuration_store::authentication();
    radiosensors::user_management::PasswordCredential credential{};
    if (!hashPassword(password, passwordLength, credential)) {
        xSemaphoreGive(managementMutex);
        sendError(request, 500, "password_hash_failed");
        return;
    }
    uint32_t createdId = 0;
    const auto mutation = radiosensors::user_management::create(
        data, username, usernameLength, role, object["enabled"] | true,
        credential, createdId);
    (void)createdId;
    memset(&credential, 0, sizeof(credential));
    if (mutation != radiosensors::user_management::Status::Ok) {
        xSemaphoreGive(managementMutex);
        sendUserMutationError(request, mutation);
        return;
    }
    const auto saved = configuration_store::saveAuthentication(data);
    xSemaphoreGive(managementMutex);
    if (saved != configuration_store::SaveStatus::Ok) {
        sendError(request, 500, "user_storage_failed");
        return;
    }
    const auto& user = data.users[data.userCount - 1];
    JsonDocument document;
    addUser(document.to<JsonObject>(), user);
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(201);
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

void handleUpdateUser(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() || !json["id"].is<uint32_t>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* username = object["username"].is<const char*>()
        ? object["username"].as<const char*>() : nullptr;
    const size_t usernameLength = username == nullptr ? 0 : strlen(username);
    radiosensors::gateway_storage::UserRole role{};
    const bool hasPassword = !object["password"].isNull();
    const char* password = hasPassword && object["password"].is<const char*>()
        ? object["password"].as<const char*>() : nullptr;
    const size_t passwordLength = password == nullptr ? 0 : strlen(password);
    if (!radiosensors::user_management::validUsername(username, usernameLength) ||
        !parseUserRole(object["role"], role) || !object["enabled"].is<bool>() ||
        (hasPassword && (passwordLength < 8 || passwordLength > 128))) {
        sendError(request, 422, "invalid_user_values");
        return;
    }
    if (managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, 409, "mutation_busy");
        return;
    }
    auto data = configuration_store::authentication();
    const uint32_t id = object["id"].as<uint32_t>();
    radiosensors::user_management::PasswordCredential credential{};
    if (hasPassword && !hashPassword(password, passwordLength, credential)) {
        xSemaphoreGive(managementMutex);
        sendError(request, 500, "password_hash_failed");
        return;
    }
    const auto mutation = radiosensors::user_management::update(
        data, id, username, usernameLength, role, object["enabled"].as<bool>(),
        hasPassword ? &credential : nullptr);
    memset(&credential, 0, sizeof(credential));
    if (mutation != radiosensors::user_management::Status::Ok) {
        xSemaphoreGive(managementMutex);
        sendUserMutationError(request, mutation);
        return;
    }
    const auto saved = configuration_store::saveAuthentication(data);
    xSemaphoreGive(managementMutex);
    if (saved != configuration_store::SaveStatus::Ok &&
        saved != configuration_store::SaveStatus::NoChange) {
        sendError(request, 500, "user_storage_failed");
        return;
    }
    authentication::invalidateUserSessions(id);
    uint8_t index = 0;
    while (data.users[index].id != id) ++index;
    const auto& user = data.users[index];
    JsonDocument document;
    addUser(document.to<JsonObject>(), user);
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

void handleDeleteUser(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() || !json["id"].is<uint32_t>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    if (managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, 409, "mutation_busy");
        return;
    }
    auto data = configuration_store::authentication();
    const uint32_t id = json["id"].as<uint32_t>();
    const auto mutation = radiosensors::user_management::remove(data, id);
    if (mutation != radiosensors::user_management::Status::Ok) {
        xSemaphoreGive(managementMutex);
        sendUserMutationError(request, mutation);
        return;
    }
    const auto saved = configuration_store::saveAuthentication(data);
    xSemaphoreGive(managementMutex);
    if (saved != configuration_store::SaveStatus::Ok) {
        sendError(request, 500, "user_storage_failed");
        return;
    }
    authentication::invalidateUserSessions(id);
    request->send(204);
}

void handleTokens(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, false)) return;
    const auto authenticationData = configuration_store::authentication();
    JsonDocument document;
    JsonArray tokens = document["tokens"].to<JsonArray>();
    for (uint8_t index = 0; index < authenticationData.tokenCount; ++index) {
        const auto& source = authenticationData.tokens[index];
        JsonObject token = tokens.add<JsonObject>();
        token["id"] = source.id;
        token["name"] = String(source.name, source.nameLength);
        token["enabled"] = source.enabled;
        token["created_at_ms"] = source.createdAtUnixMs;
        addTokenScopes(token["scopes"].to<JsonArray>(), source.scopes);
    }
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

void handleCreateToken(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() || managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, json.is<JsonObject>() ? 409 : 400,
                  json.is<JsonObject>() ? "mutation_busy" : "invalid_request");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* name = object["name"].is<const char*>()
        ? object["name"].as<const char*>() : nullptr;
    const size_t nameLength = name == nullptr ? 0 : strlen(name);
    uint16_t scopes = 0;
    if (nameLength == 0 ||
        nameLength > radiosensors::gateway_storage::kCredentialNameSize ||
        !parseTokenScopes(object["scopes"], scopes)) {
        xSemaphoreGive(managementMutex);
        sendError(request, 422, "invalid_token_values");
        return;
    }
    auto authenticationData = configuration_store::authentication();
    if (authenticationData.tokenCount >=
        radiosensors::gateway_storage::kMaximumTokens ||
        authenticationData.nextTokenId == UINT32_MAX) {
        xSemaphoreGive(managementMutex);
        sendError(request, 409, "token_capacity_reached");
        return;
    }
    char rawToken[authentication::kApiTokenCharacters + 1]{};
    auto& token = authenticationData.tokens[authenticationData.tokenCount];
    if (!authentication::createApiToken(rawToken, token.tokenHash)) {
        xSemaphoreGive(managementMutex);
        sendError(request, 500, "token_generation_failed");
        return;
    }
    token.id = authenticationData.nextTokenId == 0
        ? 1 : authenticationData.nextTokenId;
    authenticationData.nextTokenId = token.id + 1U;
    token.nameLength = static_cast<uint8_t>(nameLength);
    memcpy(token.name, name, nameLength);
    token.enabled = true;
    token.scopes = scopes;
    token.createdAtUnixMs = time_service::unixTimeMs();
    ++authenticationData.tokenCount;
    const auto saved = configuration_store::saveAuthentication(authenticationData);
    xSemaphoreGive(managementMutex);
    if (saved != configuration_store::SaveStatus::Ok) {
        memset(rawToken, 0, sizeof(rawToken));
        sendError(request, 500, "token_storage_failed");
        return;
    }

    JsonDocument document;
    document["id"] = token.id;
    document["name"] = String(token.name, token.nameLength);
    document["token"] = rawToken;
    document["created_at_ms"] = token.createdAtUnixMs;
    addTokenScopes(document["scopes"].to<JsonArray>(), token.scopes);
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(201);
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
    memset(rawToken, 0, sizeof(rawToken));
}

void handleDeleteToken(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>() || !json["id"].is<uint32_t>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    if (managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, 409, "mutation_busy");
        return;
    }
    auto authenticationData = configuration_store::authentication();
    const uint32_t id = json["id"].as<uint32_t>();
    uint8_t index = 0;
    while (index < authenticationData.tokenCount &&
           authenticationData.tokens[index].id != id) ++index;
    if (index == authenticationData.tokenCount) {
        xSemaphoreGive(managementMutex);
        sendError(request, 404, "token_not_found");
        return;
    }
    for (uint8_t move = index + 1; move < authenticationData.tokenCount; ++move) {
        authenticationData.tokens[move - 1] = authenticationData.tokens[move];
    }
    --authenticationData.tokenCount;
    memset(
        &authenticationData.tokens[authenticationData.tokenCount], 0,
        sizeof(authenticationData.tokens[0]));
    const auto saved = configuration_store::saveAuthentication(authenticationData);
    xSemaphoreGive(managementMutex);
    if (saved != configuration_store::SaveStatus::Ok) {
        sendError(request, 500, "token_storage_failed");
        return;
    }
    request->send(204);
}

void sendSettings(AsyncWebServerRequest* request) {
    const auto settings = configuration_store::settings();
    JsonDocument document;
    document["generation"] = configuration_store::settingsGeneration();
    document["hostname"] = String(settings.hostname, settings.hostnameLength);
    document["mdns_enabled"] = settings.mdnsEnabled;
    document["ntp_enabled"] = settings.ntpEnabled;
    document["pairing_window_seconds"] = settings.pairingWindowSeconds;
    document["setup_window_seconds"] = settings.setupWindowSeconds;
    JsonArray servers = document["ntp_servers"].to<JsonArray>();
    for (uint8_t index = 0; index < settings.ntpServerCount; ++index) {
        servers.add(String(
            settings.ntpServers[index], settings.ntpServerLengths[index]));
    }
    AsyncResponseStream* response =
        request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

void handleSettings(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal)) return;
    sendSettings(request);
}

void handleUpdateSettings(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    if (managementMutex == nullptr ||
        xSemaphoreTake(managementMutex, 0) != pdTRUE) {
        sendError(request, 409, "mutation_busy");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* hostname = object["hostname"].is<const char*>()
        ? object["hostname"].as<const char*>() : nullptr;
    const size_t hostnameLength = hostname == nullptr ? 0 : strlen(hostname);
    const bool shapeValid =
        object["mdns_enabled"].is<bool>() &&
        object["ntp_enabled"].is<bool>() &&
        object["pairing_window_seconds"].is<uint16_t>() &&
        object["setup_window_seconds"].is<uint16_t>() &&
        object["ntp_servers"].is<JsonArrayConst>();
    const uint16_t pairingSeconds =
        object["pairing_window_seconds"] | static_cast<uint16_t>(0);
    const uint16_t setupSeconds =
        object["setup_window_seconds"] | static_cast<uint16_t>(0);
    const JsonArrayConst servers = object["ntp_servers"].as<JsonArrayConst>();
    if (!shapeValid ||
        hostnameLength > radiosensors::gateway_storage::kHostnameSize ||
        pairingSeconds < 30 || pairingSeconds > 900 ||
        setupSeconds < 60 || setupSeconds > 1800 ||
        servers.size() > radiosensors::gateway_storage::kNtpServerCount) {
        xSemaphoreGive(managementMutex);
        sendError(request, 422, "invalid_settings_values");
        return;
    }

    auto settings = configuration_store::settings();
    memset(settings.hostname, 0, sizeof(settings.hostname));
    settings.hostnameLength = static_cast<uint8_t>(hostnameLength);
    memcpy(settings.hostname, hostname, hostnameLength);
    settings.mdnsEnabled = object["mdns_enabled"].as<bool>();
    settings.ntpEnabled = object["ntp_enabled"].as<bool>();
    settings.pairingWindowSeconds = pairingSeconds;
    settings.setupWindowSeconds = setupSeconds;
    settings.ntpServerCount = 0;
    memset(settings.ntpServerLengths, 0, sizeof(settings.ntpServerLengths));
    memset(settings.ntpServers, 0, sizeof(settings.ntpServers));
    for (const JsonVariantConst item : servers) {
        if (!item.is<const char*>()) {
            xSemaphoreGive(managementMutex);
            sendError(request, 422, "invalid_settings_values");
            return;
        }
        const char* server = item.as<const char*>();
        const size_t length = strlen(server);
        if (length == 0 ||
            length > radiosensors::gateway_storage::kNtpServerSize) {
            xSemaphoreGive(managementMutex);
            sendError(request, 422, "invalid_settings_values");
            return;
        }
        const uint8_t index = settings.ntpServerCount++;
        settings.ntpServerLengths[index] = static_cast<uint8_t>(length);
        memcpy(settings.ntpServers[index], server, length);
    }
    const auto saved = configuration_store::saveSettings(settings);
    xSemaphoreGive(managementMutex);
    if (saved == configuration_store::SaveStatus::Invalid) {
        sendError(request, 422, "invalid_settings_values");
        return;
    }
    if (saved != configuration_store::SaveStatus::Ok &&
        saved != configuration_store::SaveStatus::NoChange) {
        sendError(request, 500, "settings_storage_failed");
        return;
    }
    sendSettings(request);
}

void sendPairingStatus(AsyncWebServerRequest* request) {
    char body[128]{};
    snprintf(
        body, sizeof(body),
        "{\"active\":%s,\"remaining_seconds\":%lu,\"indication\":\"%s\"}",
        status::pairingActive() ? "true" : "false",
        static_cast<unsigned long>(status::pairingRemainingSeconds()),
        status::indicationName());
    request->send(200, "application/json", body);
}

int hexNibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decodeHex(const char* input, uint8_t* output, const size_t size) {
    if (input == nullptr || strlen(input) != size * 2) return false;
    for (size_t index = 0; index < size; ++index) {
        const int high = hexNibble(input[index * 2]);
        const int low = hexNibble(input[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void handleOpenPairing(AsyncWebServerRequest* request, JsonVariant& json) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!json.is<JsonObject>()) {
        sendError(request, 400, "invalid_request");
        return;
    }
    const JsonObject object = json.as<JsonObject>();
    const char* uidHex = object["device_uid"].is<const char*>()
        ? object["device_uid"].as<const char*>() : nullptr;
    const char* keyHex = object["factory_key"].is<const char*>()
        ? object["factory_key"].as<const char*>() : nullptr;
    uint8_t deviceUid[radiosensors::protocol::kDeviceUidSize]{};
    uint8_t factoryKey[radiosensors::gateway_storage::kFactoryKeySize]{};
    if (!decodeHex(uidHex, deviceUid, sizeof(deviceUid)) ||
        !decodeHex(keyHex, factoryKey, sizeof(factoryKey))) {
        memset(factoryKey, 0, sizeof(factoryKey));
        sendError(request, 422, "invalid_pairing_credentials");
        return;
    }
    const std::unique_ptr<registry_store::Snapshot> registry(
        new (std::nothrow) registry_store::Snapshot());
    if (!registry || !registry_store::snapshot(*registry)) {
        memset(factoryKey, 0, sizeof(factoryKey));
        sendError(request, 503, "registry_unavailable");
        return;
    }
    for (size_t index = 0; index < registry->count; ++index) {
        if (registry->records[index].state != radiosensors::registry::NodeState::Pending &&
            memcmp(registry->records[index].deviceUid, deviceUid, sizeof(deviceUid)) == 0) {
            memset(factoryKey, 0, sizeof(factoryKey));
            sendError(request, 409, "node_already_exists");
            return;
        }
    }
    const bool opened = commissioning::open(deviceUid, factoryKey);
    memset(factoryKey, 0, sizeof(factoryKey));
    if (!opened) {
        sendError(request, 503, "pairing_unavailable");
        return;
    }
    sendPairingStatus(request);
}

void handleClosePairing(AsyncWebServerRequest* request) {
    recovery::Guard recoveryGuard(0);
    if (!recoveryGuard || recovery::blocked()) {
        sendError(request, 409, "recovery_required");
        return;
    }
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!commissioning::close()) {
        sendError(request, 503, "pairing_unavailable");
        return;
    }
    sendPairingStatus(request);
}

void sendUpdateStatus(AsyncWebServerRequest* request, const int code = 200) {
    const update::Status status = update::status();
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(code);
    response->addHeader("Cache-Control", "no-store");
    response->printf(
        "{\"state\":\"%s\",\"current_version\":\"%s\",\"available_version\":\"%s\","
        "\"progress\":%u,\"error\":\"%s\",\"pending_verify\":%s}",
        update::stateName(status.state), firmware::version, status.availableVersion,
        static_cast<unsigned>(status.progress), status.error,
        status.pendingVerify ? "true" : "false");
    request->send(response);
}

void handleUpdateStatus(AsyncWebServerRequest* request) {
    // Before the first admin exists the recovery page has no session to show
    // the progress of a Web UI install with.
    authentication::Principal principal{};
    if (!status::setupRequired() && !authorizeSession(request, principal)) return;
    sendUpdateStatus(request);
}

void handleUpdateCheck(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!update::startCheck()) {
        sendError(request, 409, "update_busy");
        return;
    }
    sendUpdateStatus(request, 202);
}

void handleUpdateInstall(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeAdmin(request, principal, true)) return;
    if (!update::startInstall()) {
        sendError(request, 409, "update_unavailable");
        return;
    }
    sendUpdateStatus(request, 202);
}

// The recovery page calls this when the Web UI is missing or incompatible.
// Before the first admin exists nobody can sign in, so the physical setup
// window authorizes it instead, as it does initial setup.
void handleUpdateRepairUi(AsyncWebServerRequest* request) {
    if (status::setupRequired()) {
        if (recovery::blocked()) {
            sendError(request, 409, "recovery_required");
            return;
        }
        if (!status::setupActive()) {
            sendError(request, 403, "physical_setup_required");
            return;
        }
    } else {
        authentication::Principal principal{};
        if (!authorizeAdmin(request, principal, true)) return;
    }
    if (!update::startUiRepair()) {
        sendError(request, 409, "update_busy");
        return;
    }
    sendUpdateStatus(request, 202);
}

size_t encodeTelemetryFrame(
    const telemetry_store::Record& record,
    uint8_t* const output,
    const size_t capacity) {
    return radiosensors::stream::encodeTelemetry(
        record.sequence,
        record.nodeId,
        record.profileId,
        record.receivedAtUnixMs,
        record.rssi,
        record.data,
        record.size,
        output,
        capacity);
}

bool sendSnapshotControl(
    AsyncWebSocketClient* const client,
    const radiosensors::stream::MessageKind kind,
    const uint32_t sequence,
    const uint32_t registryGeneration) {
    uint8_t message[radiosensors::stream::kSnapshotControlFrameSize]{};
    const size_t size = radiosensors::stream::encodeSnapshotControl(
        kind, sequence, registryGeneration, message, sizeof(message));
    return size == sizeof(message) && client->binary(message, size);
}

bool sendHello(
    AsyncWebSocketClient* const client,
    const uint32_t sequence,
    const uint32_t registryGeneration) {
    uint8_t gatewayId[radiosensors::stream::kIdentitySize]{};
    uint8_t bootId[radiosensors::stream::kIdentitySize]{};
    if (!decodeHex(identity::gatewayId(), gatewayId, sizeof(gatewayId)) ||
        !decodeHex(identity::bootId(), bootId, sizeof(bootId))) {
        return false;
    }
    uint8_t message[radiosensors::stream::kHelloFrameSize]{};
    const size_t size = radiosensors::stream::encodeHello(
        sequence, gatewayId, bootId, registryGeneration,
        message, sizeof(message));
    return size == sizeof(message) && client->binary(message, size);
}

bool sendTelemetry(
    AsyncWebSocketClient* const client,
    const telemetry_store::Record& record) {
    uint8_t message[radiosensors::stream::kTelemetryEnvelopeSize + radio::kMaxPayloadSize]{};
    const size_t size = encodeTelemetryFrame(record, message, sizeof(message));
    return client->binary(message, size);
}

void sendSnapshot(AsyncWebSocketClient* const client) {
    const telemetry_store::Snapshot initial = telemetry_store::snapshot();
    const uint32_t initialRegistryGeneration = registry_store::generation();
    if (!sendHello(client, initial.updates, initialRegistryGeneration) ||
        !sendSnapshotControl(
            client,
            radiosensors::stream::MessageKind::SnapshotBegin,
            initial.updates,
            initialRegistryGeneration)) {
        client->close();
        return;
    }

    for (uint8_t nodeId = radiosensors::registry::kFirstNodeId;
         nodeId <= radiosensors::registry::kLastNodeId;
         ++nodeId) {
        telemetry_store::Record record{};
        if (telemetry_store::find(nodeId, record) &&
            !sendTelemetry(client, record)) {
            client->close();
            return;
        }
    }

    const telemetry_store::Snapshot final = telemetry_store::snapshot();
    const uint32_t finalRegistryGeneration = registry_store::generation();
    if (!sendSnapshotControl(
            client,
            radiosensors::stream::MessageKind::SnapshotEnd,
            final.updates,
            finalRegistryGeneration)) {
        client->close();
    }
}

// The name is written by whoever connects and is echoed into JSON and into the
// Web UI, so only printable ASCII survives, and the two characters that would
// break out of a JSON string never do.
void rememberStreamClient(const uint32_t id, const char* name) {
    for (StreamClientIdentity& slot : streamClients) {
        if (slot.id != 0 && slot.id != id) continue;
        slot.id = id;
        size_t written = 0;
        for (size_t index = 0;
             name != nullptr && name[index] != '\0' &&
             written < kStreamClientNameSize;
             ++index) {
            const char value = name[index];
            if (value < 0x20 || value >= 0x7F || value == '"' || value == '\\')
                continue;
            slot.name[written++] = value;
        }
        slot.name[written] = '\0';
        return;
    }
}

void forgetStreamClient(const uint32_t id) {
    for (StreamClientIdentity& slot : streamClients) {
        if (slot.id != id) continue;
        slot.id = 0;
        slot.name[0] = '\0';
        return;
    }
}

void handleWebSocketEvent(
    AsyncWebSocket*,
    AsyncWebSocketClient* client,
    const AwsEventType type,
    void* argument,
    uint8_t*,
    size_t) {
    if (type == WS_EVT_CONNECT) {
        ++websocketConnections;
        // The library holds the handshake request alive for this callback and
        // passes it here, so the identity is read where the client is known
        // and no state has to be carried over from handleHandshake.
        const auto* request = static_cast<AsyncWebServerRequest*>(argument);
        const char* name = nullptr;
        String header;
        if (request != nullptr && request->hasHeader("X-Client")) {
            header = request->getHeader("X-Client")->value();
            name = header.c_str();
        }
        rememberStreamClient(client->id(), name);
        client->setCloseClientOnQueueFull(true);
        client->keepAlivePeriod(kWebSocketKeepAliveSeconds);
        sendSnapshot(client);
    } else if (type == WS_EVT_DISCONNECT) {
        forgetStreamClient(client->id());
    }
}

// Session-only, unlike the client count in `/health`: the count says something
// is reading, which mDNS discovery already implies, while the names say which
// products this installation runs. That belongs behind a login.
void handleClients(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal, false)) return;
    JsonDocument document;
    JsonArray clients = document["clients"].to<JsonArray>();
    for (const StreamClientIdentity& slot : streamClients) {
        // Confirmed against the socket, so a slot left behind by a missed
        // disconnect is skipped rather than reported as a live client.
        if (slot.id == 0 || !telemetrySocket.hasClient(slot.id)) continue;
        JsonObject entry = clients.add<JsonObject>();
        entry["id"] = slot.id;
        entry["name"] = slot.name;
    }
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

// The public liveness probe, and deliberately almost empty. Everything a
// person would want to see about the gateway lives behind a session at
// /ui/status; this exists so something on the network can tell the gateway
// is up without a credential, and so RadioResetDialog can watch for a reboot
// after a reset has killed every session.
//
// `boot_id` is the one detail worth publishing here: it changes on every boot,
// which is what makes "it came back" distinguishable from "it never went
// down", and it is already broadcast in the mDNS TXT record anyway.
void handleHealth(AsyncWebServerRequest* request) {
    char body[96]{};
    snprintf(
        body, sizeof(body), "{\"status\":\"ok\",\"boot_id\":\"%s\"}",
        identity::bootId());
    AsyncWebServerResponse* const response =
        request->beginResponse(200, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// Everything the Web UI shows about the gateway. Session-only: it reports the
// radio pinout, MAC, counters, free heap and storage generations, which are
// diagnostics for whoever runs the gateway, not facts for the network.
void handleStatus(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal, false)) return;
    const radio::Snapshot radioSnapshot = radio::snapshot();
    const commissioning::Snapshot commissioningSnapshot = commissioning::snapshot();
    const telemetry_store::Snapshot telemetrySnapshot = telemetry_store::snapshot();
    const commands::Snapshot commandSnapshot = commands::snapshot();
    nvs_stats_t nvsStats{};
    const bool nvsStatsAvailable = nvs_get_stats(nullptr, &nvsStats) == ESP_OK;
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    response->printf(
        "{\"status\":\"ok\",\"firmware\":\"%s\",\"api_version\":%u,\"board\":\"%s\",\"hostname\":\"%s\","
        "\"gateway_id\":\"%s\",\"boot_id\":\"%s\","
        "\"reset_reason\":\"%s\",\"uptime_seconds\":%lu,\"free_heap\":%lu,"
        "\"registry\":{\"records\":%u,\"generation\":%lu},"
        "\"setup\":{\"required\":%s,\"active\":%s,\"remaining_seconds\":%lu},"
        "\"storage\":{\"ready\":%s,\"settings_generation\":%lu,"
        "\"auth_generation\":%lu,\"secrets_generation\":%lu,"
        "\"nvs\":{\"stats_available\":%s,\"used_entries\":%u,"
        "\"free_entries\":%u,\"available_entries\":%u,"
        "\"total_entries\":%u,\"namespace_count\":%u}},"
        "\"pairing\":{\"active\":%s,\"remaining_seconds\":%lu,\"indication\":\"%s\"},"
        "\"commissioning\":{\"join_requests\":%lu,\"join_accepts_queued\":%lu,"
        "\"join_confirms\":%lu,\"join_completes_queued\":%lu,"
        "\"nodes_activated\":%lu,\"rejected_frames\":%lu,"
        "\"storage_errors\":%lu,\"confirm_timeouts\":%lu},"
        "\"commands\":{\"ready\":%s,\"generation\":%lu,\"records\":%u,"
        "\"pending\":%u,\"sessions\":%lu,\"delivered\":%lu,"
        "\"no_command_replies\":%lu,\"results_recorded\":%lu,"
        "\"duplicate_results\":%lu,\"rejected_frames\":%lu,"
        "\"storage_errors\":%lu},"
        "\"ethernet\":{\"state\":\"%s\",\"has_ip\":%s,\"ip\":\"%s\","
        "\"mac\":\"%s\"},\"ota\":{\"enabled\":%s,\"state\":\"%s\",\"progress\":%u},"
        "\"web_ui\":{\"state\":\"%s\",\"version\":\"%s\","
        "\"required_firmware\":\"%s\"},"
        "\"telemetry\":{\"nodes_seen\":%u,\"updates\":%lu,"
        "\"last_node_id\":%u,\"last_received_at_ms\":%llu},"
        "\"time\":{\"state\":\"%s\",\"unix_ms\":%llu,"
        "\"last_sync_ms\":%llu},"
        "\"websocket\":{\"clients\":%u,\"connections\":%lu,"
        "\"messages_sent\":%lu,\"messages_dropped\":%lu},"
        "\"radio\":{\"state\":\"%s\",\"present\":%s,\"version\":%u,"
        "\"frequency_band_mhz\":\"%s\",\"frequency_hz\":%lu,\"bit_rate\":%lu,"
        "\"node_id\":%u,\"network_id\":%u,\"variant\":\"%s\","
        "\"configured_power_dbm\":%d,\"encryption_enabled\":%s,"
        "\"profile\":\"%s\",\"spi_host\":\"%s\","
        "\"pins\":{\"sck\":%d,\"miso\":%d,\"mosi\":%d,\"cs\":%d,\"irq\":%d,\"reset\":%d},"
        "\"counters\":{\"interrupts\":%lu,\"missed_interrupts\":%lu,"
        "\"module_restores\":%lu,\"module_restore_failures\":%lu,"
        "\"frequency_faults\":%lu,"
        "\"packets\":%lu,\"bytes\":%lu,"
        "\"empty_wakeups\":%lu,\"ack_requests_ignored\":%lu,"
        "\"telemetry_acks_sent\":%lu,\"telemetry_rejected_inactive\":%lu,"
        "\"telemetry_frames_queued\":%lu,\"telemetry_frames_dropped\":%lu,"
        "\"v3_frames\":%lu,\"v3_telemetry_frames\":%lu,"
        "\"failed_tags\":%lu,\"replay_frames\":%lu,\"activation_challenges\":%lu,"
        "\"empty_application_frames\":%lu,"
        "\"unsupported_protocol_versions\":%lu,"
        "\"unsupported_frame_kinds\":%lu,"
        "\"rx_frames_queued\":%lu,\"rx_frames_dropped\":%lu,"
        "\"session_frames_queued\":%lu,\"session_frames_dropped\":%lu,"
        "\"session_frames_rejected\":%lu,"
        "\"command_result_acks_sent\":%lu,\"command_hints_sent\":%lu,"
        "\"power_targets_sent\":%lu,"
        "\"commands_queued\":%lu,\"commands_dropped\":%lu,"
        "\"commands_processed\":%lu},"
        "\"last_packet\":{\"at_ms\":%lu,\"sender_id\":%u,\"rssi\":%d}}",
        firmware::version,
        static_cast<unsigned>(api::version),
        board::current.name,
        identity::hostname(),
        identity::gatewayId(),
        identity::bootId(),
        diagnostics::resetReason(),
        static_cast<unsigned long>(uptimeSeconds()),
        ESP.getFreeHeap(),
        static_cast<unsigned>(registry_store::recordCount()),
        static_cast<unsigned long>(registry_store::generation()),
        status::setupRequired() ? "true" : "false",
        status::setupActive() ? "true" : "false",
        static_cast<unsigned long>(status::setupRemainingSeconds()),
        configuration_store::ready() ? "true" : "false",
        static_cast<unsigned long>(configuration_store::settingsGeneration()),
        static_cast<unsigned long>(configuration_store::authenticationGeneration()),
        static_cast<unsigned long>(configuration_store::secretsGeneration()),
        nvsStatsAvailable ? "true" : "false",
        static_cast<unsigned>(nvsStats.used_entries),
        static_cast<unsigned>(nvsStats.free_entries),
        static_cast<unsigned>(nvsStats.available_entries),
        static_cast<unsigned>(nvsStats.total_entries),
        static_cast<unsigned>(nvsStats.namespace_count),
        status::pairingActive() ? "true" : "false",
        static_cast<unsigned long>(status::pairingRemainingSeconds()),
        status::indicationName(),
        commissioningSnapshot.joinRequests,
        commissioningSnapshot.joinAcceptsQueued,
        commissioningSnapshot.joinConfirms,
        commissioningSnapshot.joinCompletesQueued,
        commissioningSnapshot.nodesActivated,
        commissioningSnapshot.rejectedFrames,
        commissioningSnapshot.storageErrors,
        commissioningSnapshot.confirmTimeouts,
        commandSnapshot.ready ? "true" : "false",
        static_cast<unsigned long>(commandSnapshot.generation),
        static_cast<unsigned>(commandSnapshot.records),
        static_cast<unsigned>(commandSnapshot.pending),
        static_cast<unsigned long>(commandSnapshot.sessions),
        static_cast<unsigned long>(commandSnapshot.delivered),
        static_cast<unsigned long>(commandSnapshot.noCommandReplies),
        static_cast<unsigned long>(commandSnapshot.resultsRecorded),
        static_cast<unsigned long>(commandSnapshot.duplicateResults),
        static_cast<unsigned long>(commandSnapshot.rejectedFrames),
        static_cast<unsigned long>(commandSnapshot.storageErrors),
        ethernet::stateName(),
        ethernet::hasIp() ? "true" : "false",
        ethernet::ipAddress().c_str(),
        ethernet::macAddress().c_str(),
        ota::enabled() ? "true" : "false",
        ota::stateName(),
        ota::progressPercent(),
        web_ui::stateName(),
        web_ui::version(),
        web_ui::requiredFirmware(),
        static_cast<unsigned>(telemetrySnapshot.nodesSeen),
        static_cast<unsigned long>(telemetrySnapshot.updates),
        telemetrySnapshot.hasLast ? telemetrySnapshot.last.nodeId : 0,
        static_cast<unsigned long long>(
            telemetrySnapshot.hasLast ? telemetrySnapshot.last.receivedAtUnixMs : 0),
        time_service::stateName(),
        static_cast<unsigned long long>(time_service::unixTimeMs()),
        static_cast<unsigned long long>(time_service::lastSyncUnixMs()),
        static_cast<unsigned>(telemetrySocket.count()),
        static_cast<unsigned long>(websocketConnections.load()),
        static_cast<unsigned long>(websocketMessagesSent.load()),
        static_cast<unsigned long>(websocketMessagesDropped.load()),
        radio::stateName(),
        radioSnapshot.version == 0x24 ? "true" : "false",
        radioSnapshot.version,
        radio::frequencyBandName(),
        radioSnapshot.frequencyHz,
        radioSnapshot.bitRate,
        radio::config::nodeId,
        radioSnapshot.currentNetworkId,
        radio::config::highPower ? "HW" : "W",
        radioSnapshot.configuredPowerDbm,
        radioSnapshot.encryptionEnabled ? "true" : "false",
        radio::profileName(),
        radio::spiHostName(),
        radio::config::sck,
        radio::config::miso,
        radio::config::mosi,
        radio::config::chipSelect,
        radio::config::interrupt,
        radio::config::reset,
        radioSnapshot.interrupts,
        radioSnapshot.missedInterrupts,
        radioSnapshot.moduleRestores,
        radioSnapshot.moduleRestoreFailures,
        radioSnapshot.frequencyFaults,
        radioSnapshot.packets,
        radioSnapshot.bytes,
        radioSnapshot.emptyWakeups,
        radioSnapshot.ackRequestsIgnored,
        radioSnapshot.telemetryAcksSent,
        radioSnapshot.telemetryRejectedInactive,
        radioSnapshot.telemetryFramesQueued,
        radioSnapshot.telemetryFramesDropped,
        radioSnapshot.v3Frames,
        radioSnapshot.v3TelemetryFrames,
        radioSnapshot.failedTags,
        radioSnapshot.replayFrames,
        radioSnapshot.activationChallenges,
        radioSnapshot.emptyApplicationFrames,
        radioSnapshot.unsupportedProtocolVersions,
        radioSnapshot.unsupportedFrameKinds,
        radioSnapshot.rxFramesQueued,
        radioSnapshot.rxFramesDropped,
        radioSnapshot.sessionFramesQueued,
        radioSnapshot.sessionFramesDropped,
        radioSnapshot.sessionFramesRejected,
        radioSnapshot.commandResultAcksSent,
        radioSnapshot.commandHintsSent,
        radioSnapshot.powerTargetsSent,
        radioSnapshot.commandsQueued,
        radioSnapshot.commandsDropped,
        radioSnapshot.commandsProcessed,
        radioSnapshot.lastPacketMs,
        radioSnapshot.lastSenderId,
        radioSnapshot.lastRssi);
    const auto bench = registry_store::radioDiagnostics();
    response->printf(
        ",\"bench\":{\"last_receive_us\":%lu,\"max_receive_us\":%lu,"
        "\"reservation_writes\":%lu,\"reservation_failures\":%lu,"
        "\"last_reservation_write_us\":%lu,\"max_reservation_write_us\":%lu,"
        "\"acks_measured\":%lu,\"last_ack_us\":%lu,\"max_ack_us\":%lu,\"acks_over_40ms\":%lu,"
        "\"stack_free_bytes\":{\"radio\":%lu,\"commissioning\":%lu,\"commands\":%lu}}}",
        static_cast<unsigned long>(bench.lastReceiveUs), static_cast<unsigned long>(bench.maxReceiveUs),
        static_cast<unsigned long>(bench.reservationWrites), static_cast<unsigned long>(bench.reservationFailures),
        static_cast<unsigned long>(bench.lastReservationWriteUs), static_cast<unsigned long>(bench.maxReservationWriteUs),
        static_cast<unsigned long>(radioSnapshot.acksMeasured), static_cast<unsigned long>(radioSnapshot.lastAckUs),
        static_cast<unsigned long>(radioSnapshot.maxAckUs), static_cast<unsigned long>(radioSnapshot.acksOver40ms),
        static_cast<unsigned long>(radio::stackFreeBytes()), static_cast<unsigned long>(commissioning::stackFreeBytes()),
        static_cast<unsigned long>(commands::stackFreeBytes()));
    request->send(response);
}

void handleInfo(AsyncWebServerRequest* request) {
    if (rejectWithoutStorage(request)) return;
    JsonDocument document;
    document["firmware_version"] = firmware::version;
    document["api_version"] = api::version;
    document["stream_version"] = radiosensors::stream::kVersion;
    document["ui"]["state"] = web_ui::stateName();
    document["ui"]["version"] = web_ui::version();
    document["ui"]["required_firmware"] = web_ui::requiredFirmware();
    document["board"] = board::current.name;
    document["hostname"] = identity::hostname();
    document["gateway_id"] = identity::gatewayId();
    document["boot_id"] = identity::bootId();
    document["uptime_seconds"] = uptimeSeconds();
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(document, *response);
    request->send(response);
}

// A bench diagnostic that predates the WebSocket: it returns a real decoded
// telemetry payload, which is exactly what `telemetry:read` exists to protect,
// so it takes a session. Deliberately not a bearer endpoint -- it is not part
// of the external client contract, and a client wanting telemetry uses /ws.
void handleLastTelemetry(AsyncWebServerRequest* request) {
    authentication::Principal principal{};
    if (!authorizeSession(request, principal, false)) return;
    const telemetry_store::Snapshot telemetrySnapshot = telemetry_store::snapshot();
    if (!telemetrySnapshot.hasLast) {
        request->send(404, "application/json", "{\"error\":\"no_telemetry\"}");
        return;
    }

    const telemetry_store::Record& record = telemetrySnapshot.last;
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    response->printf(
        "{\"node_id\":%u,\"profile_id\":%u,\"received_at_ms\":%llu,"
        "\"rssi\":%d,\"size\":%u,\"sequence\":%lu,\"payload_hex\":\"",
        record.nodeId,
        record.profileId,
        static_cast<unsigned long long>(record.receivedAtUnixMs),
        record.rssi,
        record.size,
        static_cast<unsigned long>(record.sequence));
    for (uint8_t index = 0; index < record.size; ++index) {
        response->printf("%02x", record.data[index]);
    }
    response->print("\"}");
    request->send(response);
}

// REGISTRY_CHANGED, specified in WEBSOCKET.md. Polled rather than raised from
// each commit because a commit holds the registry mutex, so a callback from
// there would broadcast under that lock. The generation read is lock-free.
void broadcastRegistryChanges() {
    static uint32_t lastGeneration = 0;
    static bool primed = false;
    const uint32_t generation = registry_store::generation();
    // Boot is not a change, and a client that connects gets the registry anyway.
    if (!primed) {
        lastGeneration = generation;
        primed = true;
        return;
    }
    if (generation == lastGeneration) return;
    // Recorded before the no-clients check, so reconnecting does not replay it.
    lastGeneration = generation;
    if (telemetrySocket.count() == 0) return;
    uint8_t message[radiosensors::stream::kRegistryChangedFrameSize]{};
    const size_t size = radiosensors::stream::encodeRegistryChanged(
        telemetry_store::snapshot().updates, generation, message, sizeof(message));
    const AsyncWebSocket::SendStatus status = telemetrySocket.binaryAll(message, size);
    if (status == AsyncWebSocket::DISCARDED ||
        status == AsyncWebSocket::PARTIALLY_ENQUEUED) {
        ++websocketMessagesDropped;
    }
    if (status != AsyncWebSocket::DISCARDED) ++websocketMessagesSent;
}

}  // namespace

void begin() {
    setupMutex = xSemaphoreCreateMutex();
    managementMutex = xSemaphoreCreateMutex();
    telemetrySocket.onEvent(handleWebSocketEvent);
    telemetrySocket.handleHandshake(authorizeTelemetryStream);
    server.addHandler(&telemetrySocket);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/ui/status", HTTP_GET, handleStatus);
    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/ui/setup", HTTP_GET, handleSetupStatus);
    auto& setupHandler = server.on("/ui/setup", HTTP_POST, handleInitialSetup);
    setupHandler.setMaxContentLength(1024);
    auto& backupExport = server.on("/ui/backup/export", HTTP_POST, handleBackupExport);
    backupExport.setMaxContentLength(1024);
    auto& backupPreview = server.on("/ui/backup/preview", HTTP_POST, handleBackupImport);
    backupPreview.setMaxContentLength(kMaxBackupImportBody);
    auto& backupRestore = server.on("/ui/backup/restore", HTTP_POST, handleBackupImport);
    backupRestore.setMaxContentLength(kMaxBackupImportBody);
    auto& loginHandler = server.on(
        "/ui/session", HTTP_POST, handleLogin);
    loginHandler.setMaxContentLength(512);
    server.on("/ui/session", HTTP_GET, handleCurrentSession);
    server.on("/ui/session", HTTP_DELETE, handleLogout);
    server.on("/ui/clients", HTTP_GET, handleClients);
    server.on("/api/nodes", HTTP_GET, handleNodes);
    auto& renameNodeHandler = server.on(
        "/ui/nodes", HTTP_PATCH, handlePatchNode);
    renameNodeHandler.setMaxContentLength(256);
    auto& deleteNodeHandler = server.on(
        "/ui/nodes", HTTP_DELETE, handleDeleteNode);
    deleteNodeHandler.setMaxContentLength(128);
    server.on("/ui/settings", HTTP_GET, handleSettings);
    auto& settingsHandler = server.on(
        "/ui/settings", HTTP_PUT, handleUpdateSettings);
    settingsHandler.setMaxContentLength(1024);
    server.on("/ui/users", HTTP_GET, handleUsers);
    auto& createUserHandler = server.on(
        "/ui/users", HTTP_POST, handleCreateUser);
    createUserHandler.setMaxContentLength(512);
    auto& updateUserHandler = server.on(
        "/ui/users", HTTP_PUT, handleUpdateUser);
    updateUserHandler.setMaxContentLength(512);
    auto& deleteUserHandler = server.on(
        "/ui/users", HTTP_DELETE, handleDeleteUser);
    deleteUserHandler.setMaxContentLength(128);
    server.on("/ui/tokens", HTTP_GET, handleTokens);
    auto& createTokenHandler = server.on(
        "/ui/tokens", HTTP_POST, handleCreateToken);
    createTokenHandler.setMaxContentLength(512);
    auto& deleteTokenHandler = server.on(
        "/ui/tokens", HTTP_DELETE, handleDeleteToken);
    deleteTokenHandler.setMaxContentLength(128);
    auto& resetRadioHandler = server.on(
        "/ui/radio/reset", HTTP_POST, handleResetRadioNetwork);
    resetRadioHandler.setMaxContentLength(128);
    auto& openPairingHandler = server.on(
        "/ui/pairing/open", HTTP_POST, handleOpenPairing);
    openPairingHandler.setMaxContentLength(256);
    server.on("/ui/pairing/close", HTTP_POST, handleClosePairing);
    server.on("/ui/update", HTTP_GET, handleUpdateStatus);
    server.on("/ui/update/check", HTTP_POST, handleUpdateCheck);
    server.on("/ui/update/install", HTTP_POST, handleUpdateInstall);
    server.on("/ui/update/repair-ui", HTTP_POST, handleUpdateRepairUi);
    server.on("/ui/commands", HTTP_GET, handleCommands);
    auto& queueCommandHandler = server.on(
        "/ui/commands", HTTP_POST, handleQueueCommand);
    queueCommandHandler.setMaxContentLength(256);
    auto& cancelCommandHandler = server.on(
        "/ui/commands", HTTP_DELETE, handleCancelCommand);
    cancelCommandHandler.setMaxContentLength(128);
    server.on("/ui/telemetry/last", HTTP_GET, handleLastTelemetry);
    web_ui::addRoutes(server);
    // A mistyped API path must answer with JSON, not with the single-page
    // application, or a typo looks like a working request that returned HTML.
    server.onNotFound([](AsyncWebServerRequest* request) {
        const String url = request->url();
        if (!url.startsWith("/api/") && !url.startsWith("/ui/") &&
            web_ui::handlePageRequest(request)) return;
        request->send(404, "application/json", "{\"error\":\"not_found\"}");
    });
    server.begin();
    Serial.println("Health server listening on port 80");
}

void loop() {
    telemetrySocket.cleanupClients(kMaximumWebSocketClients);
    broadcastRegistryChanges();
    if (restartAtMs != 0 && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
        Serial.println("Gateway configuration: restarting");
        Serial.flush();
        ESP.restart();
    }
}

void publishTelemetry(const telemetry_store::Record& record) {
    if (telemetrySocket.count() == 0) return;
    uint8_t message[radiosensors::stream::kTelemetryEnvelopeSize + radio::kMaxPayloadSize]{};
    const size_t size = encodeTelemetryFrame(record, message, sizeof(message));
    const AsyncWebSocket::SendStatus status = telemetrySocket.binaryAll(message, size);
    if (status == AsyncWebSocket::DISCARDED) {
        ++websocketMessagesDropped;
    } else {
        ++websocketMessagesSent;
        if (status == AsyncWebSocket::PARTIALLY_ENQUEUED) {
            ++websocketMessagesDropped;
        }
    }
}

}  // namespace gateway::web_server
