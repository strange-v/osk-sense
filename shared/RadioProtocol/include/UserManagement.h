#pragma once

#include <GatewayStorage.h>

namespace radiosensors::user_management {

enum class Status : uint8_t {
    Ok,
    InvalidValue,
    NotFound,
    UsernameExists,
    CapacityReached,
    LastAdminRequired,
};

struct PasswordCredential {
    gateway_storage::PasswordHashAlgorithm algorithm;
    uint32_t iterations;
    uint8_t salt[gateway_storage::kPasswordSaltSize];
    uint8_t hash[gateway_storage::kPasswordHashSize];
};

bool validUsername(const char *username, size_t length);
bool validCredential(const PasswordCredential &credential);

Status create(gateway_storage::AuthenticationData &data,
              const char *username,
              size_t usernameLength,
              gateway_storage::UserRole role,
              bool enabled,
              const PasswordCredential &credential,
              uint32_t &createdId);

Status update(gateway_storage::AuthenticationData &data,
              uint32_t id,
              const char *username,
              size_t usernameLength,
              gateway_storage::UserRole role,
              bool enabled,
              const PasswordCredential *credential = nullptr);

Status remove(gateway_storage::AuthenticationData &data, uint32_t id);

} // namespace radiosensors::user_management
