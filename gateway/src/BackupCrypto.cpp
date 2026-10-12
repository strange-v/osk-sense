#include "BackupCodec.h"

#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "PasswordHashService.h"

namespace gateway::backup {
namespace {

// Magic, container version, KDF, cipher, reserved.
constexpr uint8_t kPrefix[8] = {'O', 'S', 'K', 'B', 1, 1, 1, 0};
constexpr size_t kIterationsOffset = 8;
constexpr size_t kLengthOffset = 12;
constexpr size_t kSaltOffset = 16;
constexpr size_t kSaltSize = 16;
constexpr size_t kNonceOffset = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kKeySize = 32;

uint32_t readUint32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

void writeUint32(uint8_t *bytes, const uint32_t value) {
    for (size_t index = 0; index < 4; ++index)
        bytes[index] = static_cast<uint8_t>(value >> (8 * index));
}

struct Key {
    uint8_t bytes[kKeySize]{};
    ~Key() {
        wipe(bytes, sizeof(bytes));
    }
};

struct Plaintext {
    std::string bytes;
    ~Plaintext() {
        wipe(bytes.data(), bytes.size());
    }
};

bool deriveKey(const char *password, const size_t length, const uint8_t *salt, Key &key) {
    return password_hash::computePbkdf2Sha256(password,
                                              length,
                                              salt,
                                              kSaltSize,
                                              kIterations,
                                              key.bytes,
                                              sizeof(key.bytes));
}

} // namespace

// Mirrors identity::gatewayId() so a preview names the gateway the backup restores.
void gatewayId(const Snapshot &snapshot, char output[33]) {
    constexpr char domain[] = "radiosensors-gateway-id-v1";
    constexpr size_t domainSize = sizeof(domain) - 1;
    uint8_t material[domainSize + radiosensors::gateway_storage::kDeviceSecretSize];
    memcpy(material, domain, domainSize);
    memcpy(material + domainSize,
           snapshot.secrets.deviceSecret,
           radiosensors::gateway_storage::kDeviceSecretSize);
    uint8_t digest[32]{};
    mbedtls_sha256(material, sizeof(material), digest, 0);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 16; ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0F];
    }
    output[32] = '\0';
    wipe(material, sizeof(material));
    wipe(digest, sizeof(digest));
}

bool encrypt(const Snapshot &snapshot,
             const char *password,
             const size_t length,
             std::string &file) {
    if (!validPassword(password, length))
        return false;
    Plaintext plain;
    if (!encode(snapshot, plain.bytes))
        return false;

    const size_t payloadSize = plain.bytes.size();
    file.assign(kHeaderSize + payloadSize + kTagSize, '\0');
    auto *bytes = reinterpret_cast<uint8_t *>(file.data());
    memcpy(bytes, kPrefix, sizeof(kPrefix));
    writeUint32(bytes + kIterationsOffset, kIterations);
    writeUint32(bytes + kLengthOffset, payloadSize);
    // Salt and nonce are adjacent.
    esp_fill_random(bytes + kSaltOffset, kSaltSize + kNonceSize);

    Key key;
    if (!deriveKey(password, length, bytes + kSaltOffset, key))
        return false;
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.bytes, 256);
    if (result == 0) {
        result = mbedtls_gcm_crypt_and_tag(&context,
                                           MBEDTLS_GCM_ENCRYPT,
                                           payloadSize,
                                           bytes + kNonceOffset,
                                           kNonceSize,
                                           bytes,
                                           kHeaderSize,
                                           reinterpret_cast<const uint8_t *>(plain.bytes.data()),
                                           bytes + kHeaderSize,
                                           kTagSize,
                                           bytes + kHeaderSize + payloadSize);
    }
    mbedtls_gcm_free(&context);
    if (result != 0)
        file.clear();
    return result == 0;
}

bool decrypt(const uint8_t *file,
             const size_t size,
             const char *password,
             const size_t length,
             Snapshot &snapshot) {
    if (!validPassword(password, length) || file == nullptr || size <= kHeaderSize + kTagSize ||
        size > kMaxFile || memcmp(file, kPrefix, sizeof(kPrefix)) != 0 ||
        readUint32(file + kIterationsOffset) != kIterations ||
        readUint32(file + kLengthOffset) != size - kHeaderSize - kTagSize) {
        return false;
    }
    Key key;
    if (!deriveKey(password, length, file + kSaltOffset, key))
        return false;

    const size_t payloadSize = size - kHeaderSize - kTagSize;
    Plaintext plain;
    plain.bytes.resize(payloadSize);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.bytes, 256);
    if (result == 0) {
        result = mbedtls_gcm_auth_decrypt(&context,
                                          payloadSize,
                                          file + kNonceOffset,
                                          kNonceSize,
                                          file,
                                          kHeaderSize,
                                          file + kHeaderSize + payloadSize,
                                          kTagSize,
                                          file + kHeaderSize,
                                          reinterpret_cast<uint8_t *>(plain.bytes.data()));
    }
    mbedtls_gcm_free(&context);
    return result == 0 && decode(plain.bytes.data(), plain.bytes.size(), snapshot);
}

} // namespace gateway::backup
