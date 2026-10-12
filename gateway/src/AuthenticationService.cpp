#include "AuthenticationService.h"

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include "ConfigurationStore.h"
#include "PasswordHashService.h"

namespace gateway::authentication {
namespace {

constexpr size_t kMaximumSessions = 4;
constexpr uint32_t kSessionLifetimeMs = 12UL * 60UL * 60UL * 1000UL;

struct Session {
    bool active;
    uint32_t userId;
    radiosensors::gateway_storage::UserRole role;
    char username[radiosensors::gateway_storage::kUsernameSize + 1];
    char token[kSessionTokenCharacters + 1];
    char csrfToken[kCsrfTokenCharacters + 1];
    uint32_t createdAt;
    uint32_t lastSeenAt;
};

Session sessions[kMaximumSessions]{};
SemaphoreHandle_t mutex = nullptr;

bool elapsed(const uint32_t now, const uint32_t since, const uint32_t period) {
    return static_cast<uint32_t>(now - since) >= period;
}

bool constantTimeEqual(const uint8_t *left, const uint8_t *right, const size_t size) {
    uint8_t difference = 0;
    for (size_t index = 0; index < size; ++index)
        difference |= left[index] ^ right[index];
    return difference == 0;
}

bool constantTimeEqual(const char *left, const char *right, const size_t size) {
    return constantTimeEqual(reinterpret_cast<const uint8_t *>(left),
                             reinterpret_cast<const uint8_t *>(right),
                             size);
}

bool sha256(const uint8_t *input, const size_t size, uint8_t *output) {
    const mbedtls_md_info_t *const info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr && mbedtls_md(info, input, size, output) == 0;
}

void encodeBase64Url(const uint8_t *input, const size_t size, char *output) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t inputIndex = 0;
    size_t outputIndex = 0;
    while (inputIndex + 3 <= size) {
        const uint32_t value = static_cast<uint32_t>(input[inputIndex]) << 16 |
                               static_cast<uint32_t>(input[inputIndex + 1]) << 8 |
                               input[inputIndex + 2];
        output[outputIndex++] = alphabet[(value >> 18) & 0x3F];
        output[outputIndex++] = alphabet[(value >> 12) & 0x3F];
        output[outputIndex++] = alphabet[(value >> 6) & 0x3F];
        output[outputIndex++] = alphabet[value & 0x3F];
        inputIndex += 3;
    }
    if (inputIndex < size) {
        uint32_t value = static_cast<uint32_t>(input[inputIndex]) << 16;
        if (inputIndex + 1 < size)
            value |= static_cast<uint32_t>(input[inputIndex + 1]) << 8;
        output[outputIndex++] = alphabet[(value >> 18) & 0x3F];
        output[outputIndex++] = alphabet[(value >> 12) & 0x3F];
        if (inputIndex + 1 < size)
            output[outputIndex++] = alphabet[(value >> 6) & 0x3F];
    }
    output[outputIndex] = '\0';
}

int8_t decodeBase64UrlCharacter(const char value) {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '-')
        return 62;
    if (value == '_')
        return 63;
    return -1;
}

bool decodeApiToken(const char *input, uint8_t output[32]) {
    if (input == nullptr || strlen(input) != kApiTokenCharacters)
        return false;
    uint32_t accumulator = 0;
    uint8_t bits = 0;
    size_t outputIndex = 0;
    for (size_t index = 0; index < kApiTokenCharacters; ++index) {
        const int8_t decoded = decodeBase64UrlCharacter(input[index]);
        if (decoded < 0)
            return false;
        accumulator = (accumulator << 6) | static_cast<uint8_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (outputIndex >= 32)
                return false;
            output[outputIndex++] = static_cast<uint8_t>(accumulator >> bits);
            accumulator &= bits == 0 ? 0 : (1UL << bits) - 1U;
        }
    }
    return outputIndex == 32 && accumulator == 0;
}

void randomHex(char *output, const size_t byteCount) {
    static constexpr char digits[] = "0123456789abcdef";
    uint8_t bytes[32]{};
    esp_fill_random(bytes, byteCount);
    for (size_t index = 0; index < byteCount; ++index) {
        output[index * 2] = digits[bytes[index] >> 4];
        output[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    output[byteCount * 2] = '\0';
    memset(bytes, 0, sizeof(bytes));
}

void expireSessionsLocked(const uint32_t now) {
    for (Session &session : sessions)
        if (session.active && elapsed(now, session.lastSeenAt, kSessionLifetimeMs))
            memset(&session, 0, sizeof(session));
}

Session *allocateSessionLocked(const uint32_t now) {
    expireSessionsLocked(now);
    for (Session &session : sessions)
        if (!session.active)
            return &session;
    Session *oldest = &sessions[0];
    for (Session &session : sessions)
        if (static_cast<int32_t>(session.lastSeenAt - oldest->lastSeenAt) < 0)
            oldest = &session;
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

LoginStatus createSessionLocked(const uint32_t userId,
                                const radiosensors::gateway_storage::UserRole role,
                                const char *username,
                                const size_t usernameLength,
                                LoginResult &result) {
    if (username == nullptr || usernameLength == 0 ||
        usernameLength > radiosensors::gateway_storage::kUsernameSize) {
        return LoginStatus::InvalidCredentials;
    }
    const uint32_t now = millis();
    Session *const session = allocateSessionLocked(now);
    if (session == nullptr)
        return LoginStatus::NoSessionCapacity;
    session->active = true;
    session->userId = userId;
    session->role = role;
    memcpy(session->username, username, usernameLength);
    session->username[usernameLength] = '\0';
    randomHex(session->token, kSessionTokenCharacters / 2);
    randomHex(session->csrfToken, kCsrfTokenCharacters / 2);
    session->createdAt = now;
    session->lastSeenAt = now;

    memcpy(result.sessionToken, session->token, sizeof(result.sessionToken));
    result.principal.userId = userId;
    result.principal.role = role;
    memcpy(result.principal.username, session->username, sizeof(result.principal.username));
    memcpy(result.principal.csrfToken, session->csrfToken, sizeof(result.principal.csrfToken));
    return LoginStatus::Ok;
}

} // namespace

bool begin() {
    mutex = xSemaphoreCreateMutex();
    return mutex != nullptr;
}

LoginStatus createSession(const uint32_t userId,
                          const radiosensors::gateway_storage::UserRole role,
                          const char *username,
                          const size_t usernameLength,
                          LoginResult &result) {
    if (mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return LoginStatus::Busy;
    const LoginStatus status = createSessionLocked(userId, role, username, usernameLength, result);
    xSemaphoreGive(mutex);
    return status;
}

LoginStatus login(const char *username, const char *password, LoginResult &result) {
    if (!configuration_store::ready())
        return LoginStatus::StorageUnavailable;
    if (username == nullptr || password == nullptr)
        return LoginStatus::InvalidCredentials;
    const size_t usernameLength = strlen(username);
    const size_t passwordLength = strlen(password);
    if (usernameLength == 0 || usernameLength > radiosensors::gateway_storage::kUsernameSize ||
        passwordLength < 8 || passwordLength > 128) {
        return LoginStatus::InvalidCredentials;
    }
    if (mutex == nullptr || xSemaphoreTake(mutex, 0) != pdTRUE)
        return LoginStatus::Busy;

    const auto authentication = configuration_store::authentication();
    const radiosensors::gateway_storage::UserRecord *matched = nullptr;
    for (uint8_t index = 0; index < authentication.userCount; ++index) {
        const auto &candidate = authentication.users[index];
        if (candidate.enabled && candidate.usernameLength == usernameLength &&
            memcmp(candidate.username, username, usernameLength) == 0) {
            matched = &candidate;
            break;
        }
    }

    uint8_t dummySalt[radiosensors::gateway_storage::kPasswordSaltSize]{};
    uint8_t expected[radiosensors::gateway_storage::kPasswordHashSize]{};
    const uint8_t *salt = matched == nullptr ? dummySalt : matched->salt;
    if (matched != nullptr)
        memcpy(expected, matched->passwordHash, sizeof(expected));
    const uint32_t iterations =
        matched == nullptr ? password_hash::kDefaultIterations : matched->pbkdf2Iterations;
    uint8_t computed[radiosensors::gateway_storage::kPasswordHashSize]{};
    const bool hashStatus =
        password_hash::computePbkdf2Sha256(password,
                                           passwordLength,
                                           salt,
                                           radiosensors::gateway_storage::kPasswordSaltSize,
                                           iterations,
                                           computed,
                                           sizeof(computed));
    const bool valid =
        hashStatus && matched != nullptr && constantTimeEqual(computed, expected, sizeof(computed));
    memset(computed, 0, sizeof(computed));
    memset(expected, 0, sizeof(expected));

    if (!valid) {
        xSemaphoreGive(mutex);
        return LoginStatus::InvalidCredentials;
    }
    const LoginStatus status = createSessionLocked(matched->id,
                                                   matched->role,
                                                   matched->username,
                                                   matched->usernameLength,
                                                   result);
    xSemaphoreGive(mutex);
    return status;
}

bool authorize(const char *sessionToken,
               const char *csrfToken,
               const bool requireCsrf,
               Principal &principal) {
    if (sessionToken == nullptr || strlen(sessionToken) != kSessionTokenCharacters ||
        (requireCsrf && (csrfToken == nullptr || strlen(csrfToken) != kCsrfTokenCharacters)) ||
        mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const uint32_t now = millis();
    expireSessionsLocked(now);
    for (Session &session : sessions) {
        if (!session.active ||
            !constantTimeEqual(session.token, sessionToken, kSessionTokenCharacters)) {
            continue;
        }
        if (requireCsrf && !constantTimeEqual(session.csrfToken, csrfToken, kCsrfTokenCharacters)) {
            xSemaphoreGive(mutex);
            return false;
        }
        session.lastSeenAt = now;
        principal.userId = session.userId;
        principal.role = session.role;
        memcpy(principal.username, session.username, sizeof(principal.username));
        memcpy(principal.csrfToken, session.csrfToken, sizeof(principal.csrfToken));
        xSemaphoreGive(mutex);
        return true;
    }
    xSemaphoreGive(mutex);
    return false;
}

bool logout(const char *sessionToken) {
    if (sessionToken == nullptr || strlen(sessionToken) != kSessionTokenCharacters ||
        mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    for (Session &session : sessions) {
        if (session.active &&
            constantTimeEqual(session.token, sessionToken, kSessionTokenCharacters)) {
            memset(&session, 0, sizeof(session));
            xSemaphoreGive(mutex);
            return true;
        }
    }
    xSemaphoreGive(mutex);
    return false;
}

void invalidateUserSessions(const uint32_t userId) {
    if (userId == 0 || mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return;
    for (Session &session : sessions)
        if (session.active && session.userId == userId)
            memset(&session, 0, sizeof(session));
    xSemaphoreGive(mutex);
}

bool createApiToken(char output[kApiTokenCharacters + 1],
                    uint8_t hash[radiosensors::gateway_storage::kTokenHashSize]) {
    if (output == nullptr || hash == nullptr)
        return false;
    uint8_t raw[32]{};
    esp_fill_random(raw, sizeof(raw));
    encodeBase64Url(raw, sizeof(raw), output);
    const bool hashed = sha256(raw, sizeof(raw), hash);
    memset(raw, 0, sizeof(raw));
    if (!hashed)
        memset(output, 0, kApiTokenCharacters + 1);
    return hashed;
}

bool authorizeBearer(const char *token, const uint16_t requiredScopes) {
    uint8_t raw[32]{};
    uint8_t hash[radiosensors::gateway_storage::kTokenHashSize]{};
    if (!decodeApiToken(token, raw) || !sha256(raw, sizeof(raw), hash)) {
        memset(raw, 0, sizeof(raw));
        return false;
    }
    memset(raw, 0, sizeof(raw));
    const auto authentication = configuration_store::authentication();
    bool authorized = false;
    for (uint8_t index = 0; index < authentication.tokenCount; ++index) {
        const auto &candidate = authentication.tokens[index];
        const bool hashMatches = constantTimeEqual(hash, candidate.tokenHash, sizeof(hash));
        authorized = authorized || (hashMatches && candidate.enabled &&
                                    (candidate.scopes & requiredScopes) == requiredScopes);
    }
    memset(hash, 0, sizeof(hash));
    return authorized;
}

void loop() {
    if (mutex == nullptr || xSemaphoreTake(mutex, 0) != pdTRUE)
        return;
    expireSessionsLocked(millis());
    xSemaphoreGive(mutex);
}

} // namespace gateway::authentication
