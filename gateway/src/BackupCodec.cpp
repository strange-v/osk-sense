#include "BackupCodec.h"

#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>

namespace gateway::backup {
namespace {

using namespace radiosensors;

constexpr uint8_t kPayloadVersion = 3;
constexpr size_t kRootFields = 6;
constexpr size_t kSettingsFields = 6;
constexpr size_t kNodeFields = 11;

// Wipes every block it hands back, so JSON holding keys leaves no copy in the heap.
class SecretAllocator final : public ArduinoJson::Allocator {
public:
    void* allocate(const size_t size) override {
        auto* raw = static_cast<size_t*>(malloc(sizeof(size_t) + size));
        if (raw == nullptr) return nullptr;
        *raw = size;
        return raw + 1;
    }

    void deallocate(void* pointer) override {
        if (pointer == nullptr) return;
        auto* raw = static_cast<size_t*>(pointer) - 1;
        wipe(pointer, *raw);
        free(raw);
    }

    void* reallocate(void* pointer, const size_t size) override {
        if (pointer == nullptr) return allocate(size);
        void* next = allocate(size);
        if (next == nullptr) return nullptr;
        const size_t previous = *(static_cast<size_t*>(pointer) - 1);
        memcpy(next, pointer, previous < size ? previous : size);
        deallocate(pointer);
        return next;
    }
};

std::string hex(const uint8_t* bytes, const size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        value[2 * index] = digits[bytes[index] >> 4];
        value[2 * index + 1] = digits[bytes[index] & 0x0F];
    }
    return value;
}

bool unhex(JsonVariantConst value, uint8_t* bytes, const size_t size) {
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() != size * 2) return false;
    for (size_t index = 0; index < size; ++index) {
        uint8_t result = 0;
        for (size_t digit = 0; digit < 2; ++digit) {
            const char c = text.c_str()[2 * index + digit];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            result = static_cast<uint8_t>(
                (result << 4) | (c <= '9' ? c - '0' : c - 'a' + 10));
        }
        bytes[index] = result;
    }
    return true;
}

bool stringField(
    JsonVariantConst value, char* output, const size_t capacity, uint8_t& length) {
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() > capacity || memchr(text.c_str(), 0, text.size()) != nullptr) {
        return false;
    }
    memset(output, 0, capacity);
    memcpy(output, text.c_str(), text.size());
    length = static_cast<uint8_t>(text.size());
    return true;
}

bool valid(const Snapshot& snapshot) {
    uint8_t settings[gateway_storage::kSettingsSnapshotSize]{};
    uint8_t secrets[gateway_storage::kSecretsSnapshotSize]{};
    const bool ok = snapshot.secrets.radioConfigured &&
        snapshot.secrets.deviceSecretPresent &&
        gateway_storage::encodeSettings(snapshot.settings, 0, settings, sizeof(settings)) ==
            gateway_storage::CodecStatus::Ok &&
        gateway_storage::encodeSecrets(snapshot.secrets, 0, secrets, sizeof(secrets)) ==
            gateway_storage::CodecStatus::Ok;
    wipe(secrets, sizeof(secrets));
    return ok;
}

bool decodeNode(JsonObjectConst node, registry::NodeRecord& record) {
    const JsonArrayConst firmware = node["firmware"].as<JsonArrayConst>();
    if (node.size() != kNodeFields || !node["id"].is<uint8_t>() ||
        !node["profile"].is<uint16_t>() || !node["state"].is<uint8_t>() ||
        !node["nonce"].is<uint32_t>() || !node["max_power"].is<uint8_t>() ||
        !node["replay_slot"].is<uint8_t>() ||
        !unhex(node["pairing"], record.pairing, sizeof(record.pairing)) ||
        !node["power_policy"].is<uint8_t>() || firmware.size() != 3 ||
        !firmware[0].is<uint8_t>() || !firmware[1].is<uint8_t>() ||
        !firmware[2].is<uint8_t>() ||
        !unhex(node["uid"], record.deviceUid, sizeof(record.deviceUid)) ||
        !stringField(
            node["name"], record.displayName, sizeof(record.displayName),
            record.displayNameLength)) {
        return false;
    }
    record.nodeId = node["id"].as<uint8_t>();
    record.profileId = node["profile"].as<uint16_t>();
    record.state = static_cast<registry::NodeState>(node["state"].as<uint8_t>());
    record.requestNonce = node["nonce"].as<uint32_t>();
    record.replaySlot = node["replay_slot"].as<uint8_t>();
    record.firmware = {
        firmware[0].as<uint8_t>(), firmware[1].as<uint8_t>(), firmware[2].as<uint8_t>(),
    };
    record.maxPowerLevel = node["max_power"].as<uint8_t>();
    record.powerPolicy = node["power_policy"].as<uint8_t>();
    return record.powerPolicy == 0 || record.powerPolicy - 1 <= record.maxPowerLevel;
}

}  // namespace

void wipe(void* data, size_t size) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    while (size--) *bytes++ = 0;
}

bool validPassword(const char* password, const size_t length) {
    return password != nullptr && length >= 12 && length <= 128 &&
        memchr(password, 0, length) == nullptr;
}

bool encode(const Snapshot& snapshot, std::string& output) {
    if (!valid(snapshot)) return false;
    SecretAllocator allocator;
    JsonDocument document(&allocator);
    document["version"] = kPayloadVersion;
    document["created_at_ms"] = snapshot.createdAt;

    const auto& stored = snapshot.settings;
    JsonObject settings = document["settings"].to<JsonObject>();
    settings["hostname"] = std::string(stored.hostname, stored.hostnameLength);
    settings["mdns"] = stored.mdnsEnabled;
    settings["ntp"] = stored.ntpEnabled;
    settings["pairing_seconds"] = stored.pairingWindowSeconds;
    settings["setup_seconds"] = stored.setupWindowSeconds;
    JsonArray servers = settings["servers"].to<JsonArray>();
    for (size_t index = 0; index < stored.ntpServerCount; ++index) {
        servers.add(std::string(stored.ntpServers[index], stored.ntpServerLengths[index]));
    }

    document["network_id"] = snapshot.secrets.operationalNetworkId;
    std::string secret =
        hex(snapshot.secrets.deviceSecret, sizeof(snapshot.secrets.deviceSecret));
    document["device_secret"] = secret;
    wipe(secret.data(), secret.size());

    JsonArray nodes = document["nodes"].to<JsonArray>();
    for (size_t index = 0; index < snapshot.nodes.size(); ++index) {
        const registry::NodeRecord& record = snapshot.nodes.records()[index];
        JsonObject node = nodes.add<JsonObject>();
        node["uid"] = hex(record.deviceUid, sizeof(record.deviceUid));
        node["id"] = record.nodeId;
        node["profile"] = record.profileId;
        JsonArray firmware = node["firmware"].to<JsonArray>();
        firmware.add(record.firmware.major);
        firmware.add(record.firmware.minor);
        firmware.add(record.firmware.patch);
        node["state"] = static_cast<uint8_t>(record.state);
        node["nonce"] = record.requestNonce;
        node["replay_slot"] = record.replaySlot;
        std::string pairing = hex(record.pairing, sizeof(record.pairing));
        node["pairing"] = pairing;
        wipe(pairing.data(), pairing.size());
        node["name"] = std::string(record.displayName, record.displayNameLength);
        node["max_power"] = record.maxPowerLevel;
        node["power_policy"] = record.powerPolicy;
    }
    if (document.overflowed() || measureJson(document) > kMaxPayload) return false;
    output.clear();
    serializeJson(document, output);
    return true;
}

bool decode(const char* json, const size_t size, Snapshot& snapshot) {
    if (json == nullptr || size == 0 || size > kMaxPayload) return false;
    SecretAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, json, size, DeserializationOption::NestingLimit(5)) ||
        document.overflowed()) {
        return false;
    }
    const JsonObjectConst root = document.as<JsonObjectConst>();
    if (root.size() != kRootFields || !root["version"].is<uint8_t>() ||
        root["version"].as<uint8_t>() != kPayloadVersion || !root["created_at_ms"].is<uint64_t>() ||
        !root["network_id"].is<uint8_t>() || !root["nodes"].is<JsonArrayConst>()) {
        return false;
    }
    snapshot.createdAt = root["created_at_ms"].as<uint64_t>();

    auto& secrets = snapshot.secrets;
    secrets = {};
    secrets.radioConfigured = true;
    secrets.deviceSecretPresent = true;
    secrets.operationalNetworkId = root["network_id"].as<uint8_t>();
    if (!unhex(root["device_secret"], secrets.deviceSecret,
               sizeof(secrets.deviceSecret))) {
        return false;
    }

    const JsonObjectConst settings = root["settings"].as<JsonObjectConst>();
    if (settings.size() != kSettingsFields || !settings["mdns"].is<bool>() ||
        !settings["ntp"].is<bool>() || !settings["pairing_seconds"].is<uint16_t>() ||
        !settings["setup_seconds"].is<uint16_t>() ||
        !settings["servers"].is<JsonArrayConst>()) {
        return false;
    }
    auto& stored = snapshot.settings;
    stored = {};
    if (!stringField(settings["hostname"], stored.hostname, sizeof(stored.hostname),
                     stored.hostnameLength)) {
        return false;
    }
    stored.mdnsEnabled = settings["mdns"].as<bool>();
    stored.ntpEnabled = settings["ntp"].as<bool>();
    stored.pairingWindowSeconds = settings["pairing_seconds"].as<uint16_t>();
    stored.setupWindowSeconds = settings["setup_seconds"].as<uint16_t>();
    const JsonArrayConst servers = settings["servers"].as<JsonArrayConst>();
    if (servers.size() > gateway_storage::kNtpServerCount) return false;
    for (JsonVariantConst entry : servers) {
        const size_t index = stored.ntpServerCount++;
        if (!stringField(entry, stored.ntpServers[index], sizeof(stored.ntpServers[index]),
                         stored.ntpServerLengths[index])) {
            return false;
        }
    }
    if (!valid(snapshot)) return false;

    const JsonArrayConst nodes = root["nodes"].as<JsonArrayConst>();
    if (nodes.size() > registry::kMaxNodes) return false;
    // Keep the maximum registry off the ESP32 task stack.
    auto* records = static_cast<registry::NodeRecord*>(
        calloc(registry::kMaxNodes, sizeof(registry::NodeRecord)));
    if (records == nullptr) return false;
    bool ok = true;
    size_t count = 0;
    for (JsonVariantConst node : nodes) {
        if (!decodeNode(node.as<JsonObjectConst>(), records[count++])) {
            ok = false;
            break;
        }
    }
    ok = ok && snapshot.nodes.restore(records, count);
    free(records);
    return ok;
}

}  // namespace gateway::backup
