#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "CommandSessionFrames.h"

namespace radiosensors::gateway_storage {

constexpr uint16_t kStorageVersion = 2;
constexpr size_t kSnapshotHeaderSize = 12;
constexpr size_t kSnapshotCrcSize = 4;

// A DNS label: RFC 1123 characters only, and short enough to type. Empty means
// the gateway derives `osk-hub-<mac>` instead, so a device always has a working
// name before anyone sets one.
constexpr size_t kHostnameSize = 32;
constexpr size_t kNtpServerCount = 3;
constexpr size_t kNtpServerSize = 63;
constexpr size_t kSettingsSnapshotSize = 248;

constexpr size_t kMaximumUsers = 4;
constexpr size_t kMaximumTokens = 8;
constexpr size_t kUsernameSize = 32;
constexpr size_t kCredentialNameSize = 32;
constexpr size_t kPasswordSaltSize = 16;
constexpr size_t kPasswordHashSize = 32;
constexpr size_t kTokenHashSize = 32;
constexpr size_t kStoredUserSize = 92;
constexpr size_t kStoredTokenSize = 80;
constexpr size_t kAuthSnapshotSize = 1036;

constexpr size_t kFactoryKeySize = 16;
constexpr uint16_t kSecretsStorageVersion = 3;
constexpr size_t kDeviceSecretSize = 32;
constexpr size_t kSecretsSnapshotSize = 51;
static_assert(kSecretsSnapshotSize ==
                  15 + kDeviceSecretSize + kSnapshotCrcSize,
              "installation secrets snapshot layout changed");

// The command book shares NVS with the registry and authentication, so it
// keeps pending commands and recent results within 2 x 820 bytes.
constexpr size_t kMaxCommandRecords = 16;
constexpr size_t kStoredCommandSize = 50;
constexpr size_t kCommandsSnapshotSize = 820;
static_assert(kCommandsSnapshotSize ==
                  16 + kMaxCommandRecords * kStoredCommandSize + kSnapshotCrcSize,
              "command book snapshot layout changed");

constexpr uint32_t kMinimumPbkdf2Iterations = 10000;
constexpr uint32_t kMaximumPbkdf2Iterations = 2000000;

enum class UserRole : uint8_t { Admin = 1, Viewer = 2 };
enum class PasswordHashAlgorithm : uint8_t { Pbkdf2HmacSha256 = 1 };

// One scope, because there is only one thing a bearer token can do: read the
// data a consumer needs to mirror this gateway -- the node registry and the
// telemetry stream. Splitting those in two protected nothing, because the
// WebSocket bootstrap in API.md needs the registry to resolve node IDs, so a
// stream scope without a registry scope is a combination that cannot work.
// `/health` and `/api/info` stay unauthenticated for mDNS discovery, so no
// scope guards them either.
//
// The field remains a bitfield for the day a token may write something. Today
// every mutation requires an admin session plus CSRF, so bearer tokens are
// read-only by construction and a write scope would guard nothing.
enum TokenScope : uint16_t {
    TelemetryRead = 1U << 0,
};

struct GatewaySettings {
    bool mdnsEnabled;
    bool ntpEnabled;
    uint16_t pairingWindowSeconds;
    uint16_t setupWindowSeconds;
    uint8_t hostnameLength;
    char hostname[kHostnameSize];
    uint8_t ntpServerCount;
    uint8_t ntpServerLengths[kNtpServerCount];
    char ntpServers[kNtpServerCount][kNtpServerSize];
};

struct UserRecord {
    uint32_t id;
    uint8_t usernameLength;
    char username[kUsernameSize];
    UserRole role;
    bool enabled;
    PasswordHashAlgorithm hashAlgorithm;
    uint32_t pbkdf2Iterations;
    uint8_t salt[kPasswordSaltSize];
    uint8_t passwordHash[kPasswordHashSize];
};

struct ApiTokenRecord {
    uint32_t id;
    uint8_t nameLength;
    char name[kCredentialNameSize];
    bool enabled;
    uint16_t scopes;
    uint64_t createdAtUnixMs;
    uint8_t tokenHash[kTokenHashSize];
};

struct AuthenticationData {
    uint8_t userCount;
    uint8_t tokenCount;
    uint32_t nextUserId;
    uint32_t nextTokenId;
    UserRecord users[kMaximumUsers];
    ApiTokenRecord tokens[kMaximumTokens];
};

struct InstallationSecrets {
    bool radioConfigured;
    bool deviceSecretPresent;
    uint8_t operationalNetworkId;
    uint8_t deviceSecret[kDeviceSecretSize];
};

enum class CommandState : uint8_t { Pending = 1, Completed = 2 };

// A node has at most one record: its pending command, or the result of its
// last one. The UID binds it to one physical node, because node IDs are reused.
struct CommandRecord {
    uint8_t deviceUid[protocol::kDeviceUidSize];
    uint8_t nodeId;
    uint16_t commandId;
    uint8_t type;
    uint8_t argumentSize;
    uint8_t arguments[protocol::kMaxCommandArgumentSize];
    CommandState state;
    protocol::CommandStatus status;  // zero while pending
    uint8_t resultSize;
    uint8_t result[protocol::kMaxCommandResultDataSize];
    uint64_t queuedAtUnixMs;
    uint64_t completedAtUnixMs;
};

// Records are kept oldest first.
struct CommandBook {
    uint16_t nextCommandId;
    uint8_t count;
    CommandRecord records[kMaxCommandRecords];
};

enum class CodecStatus : uint8_t {
    Ok,
    OutputTooSmall,
    InvalidSize,
    InvalidMagic,
    UnsupportedVersion,
    CrcMismatch,
    InvalidReservedData,
    InvalidFlags,
    InvalidString,
    InvalidValue,
    InvalidCount,
    DuplicateValue,
    MissingAdmin,
};

GatewaySettings defaultSettings();
AuthenticationData defaultAuthentication();
InstallationSecrets defaultSecrets();
bool settingsEqual(const GatewaySettings& left, const GatewaySettings& right);
bool authenticationEqual(const AuthenticationData& left, const AuthenticationData& right);
bool secretsEqual(const InstallationSecrets& left, const InstallationSecrets& right);

CodecStatus encodeSettings(
    const GatewaySettings& value, uint32_t generation,
    uint8_t* output, size_t capacity);
CodecStatus decodeSettings(
    const uint8_t* data, size_t size,
    GatewaySettings& value, uint32_t& generation);
CodecStatus encodeAuthentication(
    const AuthenticationData& value, uint32_t generation,
    uint8_t* output, size_t capacity);
CodecStatus decodeAuthentication(
    const uint8_t* data, size_t size,
    AuthenticationData& value, uint32_t& generation);
CodecStatus encodeSecrets(
    const InstallationSecrets& value, uint32_t generation,
    uint8_t* output, size_t capacity);
CodecStatus decodeSecrets(
    const uint8_t* data, size_t size,
    InstallationSecrets& value, uint32_t& generation);

CommandBook defaultCommandBook();
bool commandBooksEqual(const CommandBook& left, const CommandBook& right);
CodecStatus encodeCommandBook(
    const CommandBook& value, uint32_t generation,
    uint8_t* output, size_t capacity);
CodecStatus decodeCommandBook(
    const uint8_t* data, size_t size,
    CommandBook& value, uint32_t& generation);

class SlotStorage {
public:
    virtual ~SlotStorage() {}
    virtual bool exists(uint8_t slot) = 0;
    virtual bool read(uint8_t slot, uint8_t* output, size_t capacity, size_t& size) = 0;
    virtual bool write(uint8_t slot, const uint8_t* data, size_t size) = 0;
};

enum class LoadStatus : uint8_t { Loaded, Empty, Corrupt };

template<
    typename Model,
    size_t SnapshotSize,
    CodecStatus (*Encode)(const Model&, uint32_t, uint8_t*, size_t),
    CodecStatus (*Decode)(const uint8_t*, size_t, Model&, uint32_t&),
    bool (*Equal)(const Model&, const Model&),
    Model (*DefaultValue)()>
class DualSlotStore {
public:
    explicit DualSlotStore(SlotStorage& storage)
        : storage_(storage), current_{}, generation_(0), activeSlot_(-1), loaded_(false) {}

    LoadStatus load(Model& value) {
        uint8_t buffer[SnapshotSize]{};
        uint32_t newestGeneration = 0;
        int8_t newestSlot = -1;
        Model newest{};
        bool anySlotExists = false;
        for (uint8_t slot = 0; slot < 2; ++slot) {
            anySlotExists = anySlotExists || storage_.exists(slot);
            size_t size = 0;
            if (!storage_.read(slot, buffer, sizeof(buffer), size)) continue;
            Model candidate{};
            uint32_t candidateGeneration = 0;
            if (Decode(buffer, size, candidate, candidateGeneration) != CodecStatus::Ok) continue;
            if (newestSlot < 0 ||
                static_cast<int32_t>(candidateGeneration - newestGeneration) > 0) {
                newest = candidate;
                newestGeneration = candidateGeneration;
                newestSlot = static_cast<int8_t>(slot);
            }
        }
        if (newestSlot < 0) {
            current_ = DefaultValue();
            value = current_;
            generation_ = 0;
            activeSlot_ = -1;
            loaded_ = false;
            return anySlotExists ? LoadStatus::Corrupt : LoadStatus::Empty;
        }
        current_ = newest;
        value = newest;
        generation_ = newestGeneration;
        activeSlot_ = newestSlot;
        loaded_ = true;
        return LoadStatus::Loaded;
    }

    bool save(const Model& value) {
        if (loaded_ && Equal(current_, value)) return true;
        uint8_t buffer[SnapshotSize]{};
        const uint32_t nextGeneration = generation_ + 1U;
        if (Encode(value, nextGeneration, buffer, sizeof(buffer)) != CodecStatus::Ok) return false;
        const uint8_t targetSlot = activeSlot_ == 0 ? 1 : 0;
        if (!storage_.write(targetSlot, buffer, sizeof(buffer))) return false;
        size_t verifySize = 0;
        Model verified{};
        uint32_t verifiedGeneration = 0;
        if (!storage_.read(targetSlot, buffer, sizeof(buffer), verifySize) ||
            Decode(buffer, verifySize, verified, verifiedGeneration) != CodecStatus::Ok ||
            verifiedGeneration != nextGeneration || !Equal(verified, value)) return false;
        current_ = value;
        generation_ = nextGeneration;
        activeSlot_ = static_cast<int8_t>(targetSlot);
        loaded_ = true;
        return true;
    }

    uint32_t generation() const { return generation_; }

private:
    SlotStorage& storage_;
    Model current_;
    uint32_t generation_;
    int8_t activeSlot_;
    bool loaded_;
};

using SettingsStore = DualSlotStore<GatewaySettings, kSettingsSnapshotSize,
    encodeSettings, decodeSettings, settingsEqual, defaultSettings>;
using AuthenticationStore = DualSlotStore<AuthenticationData, kAuthSnapshotSize,
    encodeAuthentication, decodeAuthentication, authenticationEqual, defaultAuthentication>;
using SecretsStore = DualSlotStore<InstallationSecrets, kSecretsSnapshotSize,
    encodeSecrets, decodeSecrets, secretsEqual, defaultSecrets>;
using CommandStore = DualSlotStore<CommandBook, kCommandsSnapshotSize,
    encodeCommandBook, decodeCommandBook, commandBooksEqual, defaultCommandBook>;

}  // namespace radiosensors::gateway_storage
