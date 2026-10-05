#include "BackupService.h"
#include "RecoveryService.h"
#include "PasswordHashService.h"
#include "ConfigurationStore.h"
#include "NodeRegistryStore.h"
#include "GatewayStatus.h"
#include "OtaService.h"
#include "TimeService.h"
#include <ResetButton.h>
#include <RegistryPersistence.h>
#include <ArduinoJson.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/gcm.h>
#include <nvs.h>
#include <cassert>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>

using namespace gateway;
using namespace radiosensors;
using Bytes = std::vector<uint8_t>;
using Database = std::map<std::string, std::map<std::string, Bytes>>;
Database flash;
std::map<unsigned, std::string> handles;
int writes = 0, cutAt = -1;
bool cutBefore = false, badReadback = false;
const char* fixturePath = nullptr;
struct PowerCut {};
struct Restart {};
TestEsp ESP;
void TestEsp::restart() { throw Restart{}; }
uint32_t millis() { return 100; }
void beforeWrite() { if (cutBefore && ++writes == cutAt) throw PowerCut{}; }
void afterWrite() { if (!cutBefore && ++writes == cutAt) throw PowerCut{}; }
esp_err_t nvs_open(const char* name, int, nvs_handle_t* handle) { *handle = handles.size() + 1; handles[*handle] = name; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t value) { beforeWrite(); flash[handles[h]][key] = {value}; afterWrite(); return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* value) {
    auto& space = flash[handles[h]];
    if (!space.count(key)) return ESP_ERR_NVS_NOT_FOUND;
    *value = space[key][0]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* p, size_t n) {
    beforeWrite(); const auto* b = static_cast<const uint8_t*>(p); flash[handles[h]][key] = Bytes(b, b+n); afterWrite(); return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* p, size_t* n) {
    if (badReadback) return -1;
    const auto& b = flash[handles[h]][key];
    if (*n < b.size()) return -1;
    *n = b.size(); memcpy(p, b.data(), *n); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { beforeWrite(); afterWrite(); return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { beforeWrite(); flash[handles[h]].clear(); afterWrite(); return ESP_OK; }

// Deterministic entropy is confined to this native test adapter.
void esp_fill_random(void* p, size_t size) { static uint8_t value = 0; auto* b = static_cast<uint8_t*>(p); while (size--) *b++ = ++value; }
namespace gateway::password_hash {
bool computePbkdf2Sha256(const char* p, size_t n, const uint8_t* salt, size_t sn, uint32_t iterations, uint8_t* output, size_t size) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(p), n, salt, sn, iterations, size, output) == 0;
}
}
bool clean = true, physical = true;
namespace gateway::configuration_store {
bool ready() { return true; }
gateway_storage::GatewaySettings settings() { return gateway_storage::defaultSettings(); }
gateway_storage::AuthenticationData authentication() { auto a = gateway_storage::defaultAuthentication(); a.userCount = clean ? 0 : 1; return a; }
gateway_storage::InstallationSecrets secrets() { return gateway_storage::defaultSecrets(); }
}
namespace gateway::registry_store {
size_t recordCount() { return 0; }
bool snapshot(Snapshot& s) { s.count = 0; return true; }
}
namespace gateway::status { bool setupActive() { return physical; } bool pairingActive() { return false; } }
namespace gateway::ota { State state() { return State::Ready; } }
namespace gateway::time_service { uint64_t unixTimeMs() { return 0; } }

void fixture(backup::Snapshot& s) {
    s.settings = gateway_storage::defaultSettings();
    memcpy(s.settings.hostname, "greenhouse", 10); s.settings.hostnameLength = 10;
    s.secrets = gateway_storage::defaultSecrets();
    s.secrets.deviceSecretPresent = s.secrets.installationKeyPresent = true;
    s.secrets.operationalNetworkId = 123;
    for (size_t i = 0; i < 32; ++i) s.secrets.deviceSecret[i] = i;
    for (size_t i = 0; i < 16; ++i) s.secrets.installationKey[i] = i * 7;
    s.createdAt = 1700000000000;
    registry::NodeRecord record{};
    record.nodeId = 7; record.deviceUid[0] = 4; record.profileId = 6;
    record.firmware = {1, 2, 3}; record.state = registry::NodeState::Active;
    record.maxPowerLevel = 2; record.powerPolicy = 2; record.requestNonce = 42;
    security::pairing::Record pairing;
    pairing.state = security::pairing::State::Active;
    pairing.requestTransport = {100,0,0}; pairing.acceptTransport = {0,100,0};
    security::frames::JoinRequest request;
    memcpy(request.identity.uid, record.deviceUid, sizeof(record.deviceUid));
    request.identity.requestNonce = 0xFEDCBA980000002AULL;
    request.profileId = record.profileId; request.firmware = record.firmware; request.maxPowerLevel = 2;
    uint8_t factory[16]{}; osk::crypto::Cmac mac; osk::crypto::cmacInit(mac, factory);
    assert(security::frames::sealJoinRequest(mac, pairing.requestTransport, request, pairing.request, sizeof(pairing.request)));
    security::frames::JoinAccept accept;
    accept.identity = request.identity; accept.nodeId = record.nodeId; accept.networkId = 123;
    accept.salt[0] = 7;
    assert(security::frames::sealJoinAccept(mac, pairing.acceptTransport, accept, pairing.accept, sizeof(pairing.accept)));
    security::deriveKeys(factory, accept.salt, pairing.keys, mac);
    record.replaySlot = 3;
    assert(security::pairing::encode(pairing, 2, record.pairing, sizeof(record.pairing)));
    const std::string name = u8"Лічильник";
    record.displayNameLength = name.size(); memcpy(record.displayName, name.data(), name.size());
    assert(s.nodes.restore(&record, 1));
}
gateway_storage::AuthenticationData admin() {
    auto a = gateway_storage::defaultAuthentication(); a.userCount = 1; a.nextUserId = 2;
    auto& u = a.users[0]; u.id = 1; u.usernameLength = 5; memcpy(u.username, "admin", 5);
    u.enabled = true; u.role = gateway_storage::UserRole::Admin;
    u.hashAlgorithm = gateway_storage::PasswordHashAlgorithm::Pbkdf2HmacSha256; u.pbkdf2Iterations = 25000;
    u.salt[0] = 1; u.passwordHash[0] = 1;
    u.salt[0] = 1; u.passwordHash[0] = 1;
    return a;
}
void assertRestored(const backup::Snapshot& expected) {
    uint32_t generation;
    gateway_storage::InstallationSecrets secrets;
    const auto& raw = flash["gateway-secrets"]["secret_a"];
    assert(gateway_storage::decodeSecrets(raw.data(), raw.size(), secrets, generation) == gateway_storage::CodecStatus::Ok);
    assert(gateway_storage::secretsEqual(secrets, expected.secrets));
    registry::NodeRegistry nodes;
    const auto& reg = flash["node-reg"]["registry"];
    assert(registry::decodeRegistrySnapshot(reg.data(), reg.size(), nodes, generation) == registry::SnapshotStatus::Ok);
    assert(nodes.size() == expected.nodes.size() && nodes.records()[0].nodeId == 7);
    assert(nodes.records()[0].replaySlot == expected.nodes.records()[0].replaySlot);
    assert(!memcmp(nodes.records()[0].pairing, expected.nodes.records()[0].pairing, security::pairing::kSnapshotSize));
    gateway_storage::GatewaySettings settings;
    const auto& config = flash["gateway-config"]["config_a"];
    assert(gateway_storage::decodeSettings(config.data(), config.size(), settings, generation) == gateway_storage::CodecStatus::Ok);
    assert(gateway_storage::settingsEqual(settings, expected.settings));
    gateway_storage::AuthenticationData auth;
    const auto& authBytes = flash["gateway-auth"]["auth_a"];
    assert(gateway_storage::decodeAuthentication(authBytes.data(), authBytes.size(), auth, generation) == gateway_storage::CodecStatus::Ok);
    assert(auth.userCount == 1 && auth.tokenCount == 0 && auth.users[0].id == 1);
    assert(flash["node-cmd"].empty());
    assert(flash["radio-bound"].empty());
}
void codecAndCrypto() {
    backup::Snapshot s, decoded; fixture(s);
    std::string json; assert(backup::encode(s, json));
    assert(json.find("password") == std::string::npos && json.find("token") == std::string::npos);
    assert(backup::decode(json.data(), json.size(), decoded));
    assert(gateway_storage::settingsEqual(s.settings, decoded.settings));
    assert(gateway_storage::secretsEqual(s.secrets, decoded.secrets));
    assert(decoded.nodes.records()[0].nodeId == 7);
    assert(decoded.nodes.records()[0].replaySlot == 3);
    assert(!memcmp(s.nodes.records()[0].pairing, decoded.nodes.records()[0].pairing, security::pairing::kSnapshotSize));
    JsonDocument doc; assert(!deserializeJson(doc, json));
    auto rejects = [&]() { std::string text; serializeJson(doc, text); assert(!backup::decode(text.data(), text.size(), decoded)); };
    doc["version"] = 1; rejects(); doc["version"] = 2;
    doc["network_id"] = 0; rejects(); doc["network_id"] = 123;
    doc["settings"]["pairing_seconds"] = "120"; rejects(); doc["settings"]["pairing_seconds"] = 120;
    doc["nodes"][0]["power_policy"] = 4; rejects(); doc["nodes"][0]["power_policy"] = 2;
    doc["nodes"].as<JsonArray>().add(doc["nodes"][0]); rejects();
    std::string oversized(backup::kMaxPayload + 1, ' '); assert(!backup::decode(oversized.data(), oversized.size(), decoded));
    constexpr char password[] = "backup test password";
    std::string file, second;
    assert(backup::encrypt(s, password, strlen(password), file));
    assert(backup::encrypt(s, password, strlen(password), second) && file != second);
    auto decrypt = [&](const std::string& data, const char* p = "backup test password") {
        return backup::decrypt(reinterpret_cast<const uint8_t*>(data.data()), data.size(), p, strlen(p), decoded);
    };
    assert(decrypt(file));
    assert(!decrypt(file, "wrong test password"));
    assert(!decrypt(file.substr(0, file.size()-1)));
    for (size_t offset : {size_t(0), size_t(4), size_t(7), size_t(8), size_t(12), size_t(16), size_t(32), backup::kHeaderSize, file.size()-1}) {
        auto damaged = file; damaged[offset] ^= 1; assert(!decrypt(damaged));
    }
    char id[33], restoredId[33]; backup::gatewayId(s, id); backup::gatewayId(decoded, restoredId); assert(!strcmp(id, restoredId));
    if (fixturePath) std::ofstream(fixturePath, std::ios::binary).write(file.data(), file.size());
    // The largest valid installation must fit kMaxPayload.
    s.settings.hostnameLength = 32; memset(s.settings.hostname, 'a', 32);
    s.settings.ntpServerCount = 3;
    for (size_t i = 0; i < 3; ++i) { s.settings.ntpServerLengths[i] = 63; memset(s.settings.ntpServers[i], 'a' + i, 63); }
    s.settings.pairingWindowSeconds = 900; s.settings.setupWindowSeconds = 1800;
    s.secrets.operationalNetworkId = 255; s.createdAt = UINT64_MAX;
    registry::NodeRecord records[registry::kMaxNodes]{};
    for (size_t i = 0; i < registry::kMaxNodes; ++i) {
        records[i] = s.nodes.records()[0]; records[i].nodeId = 36 + i; records[i].deviceUid[0] = i+1;
        records[i].profileId = UINT16_MAX; records[i].firmware = {255, 255, 255};
        records[i].state = registry::NodeState::Disabled; records[i].requestNonce = UINT32_MAX;
        records[i].maxPowerLevel = 31; records[i].powerPolicy = 32;
        records[i].displayNameLength = 48; memset(records[i].displayName, '"', 48);
        security::pairing::Record pairing; uint32_t generation = 0;
        assert(security::pairing::decode(records[i].pairing, sizeof(records[i].pairing), pairing, generation));
        memcpy(pairing.request + 1, records[i].deviceUid, sizeof(records[i].deviceUid));
        memcpy(pairing.accept + 1, records[i].deviceUid, sizeof(records[i].deviceUid));
        protocol::writeUint32Le(pairing.request + 11, records[i].requestNonce);
        protocol::writeUint32Le(pairing.accept + 11, records[i].requestNonce);
        pairing.accept[27] = records[i].nodeId;
        records[i].replaySlot = i;
        assert(security::pairing::encode(pairing, generation, records[i].pairing, sizeof(records[i].pairing)));
    }
    assert(s.nodes.restore(records, registry::kMaxNodes)); assert(backup::encode(s, json));
    assert(backup::decode(json.data(), json.size(), decoded) && decoded.nodes.size() == registry::kMaxNodes);
    std::cout << "Codec, maximum registry, tampering, password and crypto round-trip passed\n";
}
void restorePowerCuts() {
    backup::Snapshot s; fixture(s); const auto auth = admin();
    flash.clear(); flash["radio-bound"]["bounds"] = {1,2,3}; recovery::begin(); writes = 0;
    assert(backup::restore(s, auth)); const int total = writes;
    recovery::begin(); assert(!recovery::blocked()); assertRestored(s);
    for (bool before : {false, true}) for (int point = 1; point <= total; ++point) {
        flash.clear(); flash["radio-bound"]["bounds"] = {1,2,3}; cutAt = -1; recovery::begin();
        writes = 0; cutAt = point; cutBefore = before;
        try { backup::restore(s, auth); assert(false); } catch (const PowerCut&) {}
        cutAt = -1; recovery::begin();
        if (!recovery::blocked()) {
            if (!flash["gateway-secrets"]["secret_a"].empty()) assertRestored(s);
            else assert(flash["node-reg"].empty());
        } else {
            assert(!backup::cleanGateway());
            try { recovery::requestFactoryReset(); } catch (const Restart&) {}
            recovery::begin(); assert(!recovery::blocked());
            assert(backup::restore(s, auth)); recovery::begin(); assertRestored(s);
        }
    }
    flash.clear(); recovery::begin(); badReadback = true;
    assert(!backup::restore(s, auth)); assert(recovery::blocked());
    badReadback = false; recovery::begin(); assert(recovery::blocked());
    flash.clear(); recovery::begin(); clean = false; writes = 0;
    assert(!backup::restore(s, auth) && writes == 0); clean = true;
    physical = false; assert(!backup::restore(s, auth) && writes == 0); physical = true;
    std::cout << "Restore: " << total * 2 << " power-cut boundaries, read-back failure and access gates passed\n";
}
void resetPowerCuts() {
    Database initial;
    for (const char* name : {"gateway-config", "gateway-auth", "gateway-secrets", "node-reg", "node-cmd", "radio-bound"}) initial[name]["old"] = {1,2,3};
    initial["gateway-recover"]["operation"] = {2};
    flash = initial; writes = 0; recovery::begin(); const int total = writes;
    for (bool before : {false, true}) for (int point = 1; point <= total; ++point) {
        flash = initial; writes = 0; cutAt = point; cutBefore = before;
        try { recovery::begin(); assert(false); } catch (const PowerCut&) {}
        cutAt = -1; recovery::begin(); assert(!recovery::blocked());
        for (const auto& entry : initial) if (entry.first != "gateway-recover") assert(flash[entry.first].empty());
    }
    std::cout << "Reset: " << total * 2 << " power-cut boundaries passed\n";
}
void buttonTests() {
    using A = ResetButton::Action;
    ResetButton b;
    assert(b.update(true, 100) == A::None); assert(b.update(false, 300) == A::ShortPress);
    b.update(true, 1000); b.update(true, 10999); assert(!b.confirming());
    b.update(true, 11000); assert(b.confirming());
    b.update(true, 20000); assert(b.confirming()); b.update(false, 20001);
    assert(b.update(true, 24000) == A::ConfirmReset); assert(b.update(false, 24100) == A::None);
    b.update(true, 30000); b.update(true, 40000); b.update(false, 40001); b.update(false, 45001); assert(!b.confirming());
    assert(b.update(true, 46000) == A::None);
    ResetButton wrap; wrap.update(true, UINT32_MAX - 5000); wrap.update(true, 5000); assert(wrap.confirming());
    std::cout << "Button: short/long press, release, confirmation expiry and clock wrap passed\n";
}
int main(int argc, char** argv) {
    // Optional: where to write an encrypted fixture for check_interop.mjs.
    fixturePath = argc > 1 ? argv[1] : nullptr;
    codecAndCrypto(); restorePowerCuts(); resetPowerCuts(); buttonTests();
}
