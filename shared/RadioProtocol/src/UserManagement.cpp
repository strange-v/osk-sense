#include "UserManagement.h"

#include <string.h>

namespace radiosensors::user_management {
namespace {

using namespace gateway_storage;

bool validRole(const UserRole role) {
    return role == UserRole::Admin || role == UserRole::Viewer;
}

bool allZero(const uint8_t *value, const size_t size) {
    uint8_t combined = 0;
    for (size_t index = 0; index < size; ++index)
        combined |= value[index];
    return combined == 0;
}

bool usernameExists(const AuthenticationData &data,
                    const char *username,
                    const size_t length,
                    const uint32_t exceptId = 0) {
    for (uint8_t index = 0; index < data.userCount; ++index) {
        const UserRecord &user = data.users[index];
        if (user.id != exceptId && user.usernameLength == length &&
            memcmp(user.username, username, length) == 0)
            return true;
    }
    return false;
}

bool hasEnabledAdmin(const AuthenticationData &data) {
    for (uint8_t index = 0; index < data.userCount; ++index) {
        const UserRecord &user = data.users[index];
        if (user.enabled && user.role == UserRole::Admin)
            return true;
    }
    return false;
}

void applyCredential(UserRecord &user, const PasswordCredential &credential) {
    user.hashAlgorithm = credential.algorithm;
    user.pbkdf2Iterations = credential.iterations;
    memcpy(user.salt, credential.salt, sizeof(user.salt));
    memcpy(user.passwordHash, credential.hash, sizeof(user.passwordHash));
}

} // namespace

bool validUsername(const char *username, const size_t length) {
    if (username == nullptr || length == 0 || length > kUsernameSize)
        return false;
    for (size_t index = 0; index < length; ++index) {
        const char character = username[index];
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
              character == '.' || character == '_' || character == '-'))
            return false;
    }
    return true;
}

bool validCredential(const PasswordCredential &credential) {
    return credential.algorithm == PasswordHashAlgorithm::Pbkdf2HmacSha256 &&
           credential.iterations >= kMinimumPbkdf2Iterations &&
           credential.iterations <= kMaximumPbkdf2Iterations &&
           !allZero(credential.salt, sizeof(credential.salt)) &&
           !allZero(credential.hash, sizeof(credential.hash));
}

Status create(AuthenticationData &data,
              const char *username,
              const size_t usernameLength,
              const UserRole role,
              const bool enabled,
              const PasswordCredential &credential,
              uint32_t &createdId) {
    createdId = 0;
    if (!validUsername(username, usernameLength) || !validRole(role) ||
        !validCredential(credential))
        return Status::InvalidValue;
    if (data.userCount >= kMaximumUsers || data.nextUserId == UINT32_MAX)
        return Status::CapacityReached;
    if (usernameExists(data, username, usernameLength))
        return Status::UsernameExists;
    AuthenticationData candidate = data;
    UserRecord &user = candidate.users[candidate.userCount];
    user.id = candidate.nextUserId == 0 ? 1 : candidate.nextUserId;
    candidate.nextUserId = user.id + 1U;
    user.usernameLength = static_cast<uint8_t>(usernameLength);
    memcpy(user.username, username, usernameLength);
    user.role = role;
    user.enabled = enabled;
    applyCredential(user, credential);
    ++candidate.userCount;
    if (!hasEnabledAdmin(candidate))
        return Status::LastAdminRequired;
    createdId = user.id;
    data = candidate;
    return Status::Ok;
}

Status update(AuthenticationData &data,
              const uint32_t id,
              const char *username,
              const size_t usernameLength,
              const UserRole role,
              const bool enabled,
              const PasswordCredential *credential) {
    if (!validUsername(username, usernameLength) || !validRole(role) ||
        (credential != nullptr && !validCredential(*credential)))
        return Status::InvalidValue;
    uint8_t index = 0;
    while (index < data.userCount && data.users[index].id != id)
        ++index;
    if (index == data.userCount)
        return Status::NotFound;
    if (usernameExists(data, username, usernameLength, id))
        return Status::UsernameExists;
    AuthenticationData candidate = data;
    UserRecord &user = candidate.users[index];
    memset(user.username, 0, sizeof(user.username));
    memcpy(user.username, username, usernameLength);
    user.usernameLength = static_cast<uint8_t>(usernameLength);
    user.role = role;
    user.enabled = enabled;
    if (credential != nullptr)
        applyCredential(user, *credential);
    if (!hasEnabledAdmin(candidate))
        return Status::LastAdminRequired;
    data = candidate;
    return Status::Ok;
}

Status remove(AuthenticationData &data, const uint32_t id) {
    uint8_t index = 0;
    while (index < data.userCount && data.users[index].id != id)
        ++index;
    if (index == data.userCount)
        return Status::NotFound;
    AuthenticationData candidate = data;
    for (uint8_t move = index + 1; move < candidate.userCount; ++move)
        candidate.users[move - 1] = candidate.users[move];
    --candidate.userCount;
    memset(&candidate.users[candidate.userCount], 0, sizeof(candidate.users[0]));
    if (!hasEnabledAdmin(candidate))
        return Status::LastAdminRequired;
    data = candidate;
    return Status::Ok;
}

} // namespace radiosensors::user_management
