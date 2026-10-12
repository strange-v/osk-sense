#pragma once

#include <GatewayStorage.h>
#include <stddef.h>
#include <stdint.h>

namespace gateway::authentication {

constexpr size_t kSessionTokenCharacters = 64;
constexpr size_t kCsrfTokenCharacters = 32;
constexpr size_t kApiTokenCharacters = 43;

enum class LoginStatus : uint8_t {
    Ok,
    InvalidCredentials,
    Busy,
    StorageUnavailable,
    NoSessionCapacity,
};

struct Principal {
    uint32_t userId;
    radiosensors::gateway_storage::UserRole role;
    char username[radiosensors::gateway_storage::kUsernameSize + 1];
    char csrfToken[kCsrfTokenCharacters + 1];
};

struct LoginResult {
    char sessionToken[kSessionTokenCharacters + 1];
    Principal principal;
};

bool begin();
LoginStatus login(const char *username, const char *password, LoginResult &result);
LoginStatus createSession(uint32_t userId,
                          radiosensors::gateway_storage::UserRole role,
                          const char *username,
                          size_t usernameLength,
                          LoginResult &result);
bool authorize(const char *sessionToken,
               const char *csrfToken,
               bool requireCsrf,
               Principal &principal);
bool logout(const char *sessionToken);
void invalidateUserSessions(uint32_t userId);
bool createApiToken(char output[kApiTokenCharacters + 1],
                    uint8_t hash[radiosensors::gateway_storage::kTokenHashSize]);
bool authorizeBearer(const char *token, uint16_t requiredScopes);
void loop();

} // namespace gateway::authentication
