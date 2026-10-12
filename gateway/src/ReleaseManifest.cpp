#include "ReleaseManifest.h"

#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace gateway::release {
namespace {

constexpr uint8_t kManifestFormat = 2;

bool parsePart(const char *&cursor, uint16_t &value) {
    if (*cursor < '0' || *cursor > '9')
        return false;
    if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')
        return false;
    uint32_t result = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        result = result * 10 + static_cast<uint32_t>(*cursor - '0');
        if (result > UINT16_MAX)
            return false;
        ++cursor;
    }
    value = static_cast<uint16_t>(result);
    return true;
}

int hexValue(const char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    return -1;
}

// The file name becomes part of a download URL, so it is limited to
// characters that need no escaping and cannot change the path.
bool validFileName(const char *name) {
    const size_t length = strlen(name);
    if (length == 0 || length > kMaxFileNameLength || name[0] == '.')
        return false;
    for (size_t index = 0; index < length; ++index) {
        const char c = name[index];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
            return false;
    }
    return true;
}

bool positive(JsonVariantConst value) {
    return value.is<uint32_t>() && value.as<uint32_t>() != 0;
}

// Firmware: file, size, sha256. Compressed Web UI: also image_size.
bool readImage(JsonObjectConst object, const bool compressed, Image &image) {
    const char *file = object["file"].as<const char *>();
    const char *sha256 = object["sha256"].as<const char *>();
    if (object.size() != (compressed ? 4U : 3U) || file == nullptr || !validFileName(file) ||
        !positive(object["size"]) || (compressed && !positive(object["image_size"])) ||
        sha256 == nullptr || strlen(sha256) != kSha256Size * 2) {
        return false;
    }
    for (size_t index = 0; index < kSha256Size; ++index) {
        const int high = hexValue(sha256[index * 2]);
        const int low = hexValue(sha256[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        image.sha256[index] = static_cast<uint8_t>((high << 4) | low);
    }
    memcpy(image.file, file, strlen(file) + 1);
    image.size = object["size"].as<uint32_t>();
    image.imageSize = compressed ? object["image_size"].as<uint32_t>() : image.size;
    return true;
}

} // namespace

bool parseVersion(const char *text, Version &version) {
    if (text == nullptr)
        return false;
    const char *cursor = text;
    Version result;
    if (!parsePart(cursor, result.major) || *cursor++ != '.' || !parsePart(cursor, result.minor) ||
        *cursor++ != '.' || !parsePart(cursor, result.patch) || *cursor != '\0') {
        return false;
    }
    version = result;
    return true;
}

int compare(const Version &left, const Version &right) {
    if (left.major != right.major)
        return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor)
        return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch)
        return left.patch < right.patch ? -1 : 1;
    return 0;
}

bool verifySignature(const uint8_t *manifest,
                     const size_t size,
                     const uint8_t *signature,
                     const size_t signatureSize) {
    if (manifest == nullptr || signature == nullptr || signatureSize == 0 ||
        signatureSize > kMaxSignatureSize) {
        return false;
    }
    uint8_t hash[kSha256Size];
    if (mbedtls_sha256(manifest, size, hash, 0) != 0)
        return false;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    // PEM parsing needs the terminating NUL counted in the length.
    const bool valid =
        mbedtls_pk_parse_public_key(&key,
                                    reinterpret_cast<const unsigned char *>(kSigningPublicKey),
                                    sizeof(kSigningPublicKey)) == 0 &&
        mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) &&
        mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, signatureSize) ==
            0;
    mbedtls_pk_free(&key);
    return valid;
}

Status parse(const uint8_t *manifest,
             const size_t size,
             const char *board,
             const char *currentVersion,
             Release &release) {
    if (manifest == nullptr || board == nullptr)
        return Status::Malformed;
    if (size > kMaxManifestSize)
        return Status::TooLarge;

    JsonDocument document;
    if (deserializeJson(document, reinterpret_cast<const char *>(manifest), size) !=
            DeserializationError::Ok ||
        !document.is<JsonObjectConst>()) {
        return Status::Malformed;
    }
    if (!document["format"].is<uint8_t>())
        return Status::Malformed;
    if (document["format"].as<uint8_t>() != kManifestFormat)
        return Status::UnsupportedFormat;

    Release result;
    const char *versionText = document["version"].as<const char *>();
    if (!parseVersion(versionText, result.version) || !document["boards"].is<JsonObjectConst>())
        return Status::Malformed;
    memcpy(result.versionText, versionText, strlen(versionText) + 1);

    const JsonVariantConst boardEntry = document["boards"][board];
    if (boardEntry.isNull())
        return Status::BoardMissing;
    const JsonObjectConst entry = boardEntry.as<JsonObjectConst>();
    if (entry.isNull() || entry.size() != 2 ||
        !readImage(entry["firmware"], false, result.firmware) ||
        !readImage(entry["ui"], true, result.ui)) {
        return Status::Malformed;
    }

    Version current;
    if (!parseVersion(currentVersion, current))
        return Status::Malformed;
    if (compare(result.version, current) <= 0)
        return Status::NotNewer;

    release = result;
    return Status::Ok;
}

Status read(const uint8_t *manifest,
            const size_t size,
            const uint8_t *signature,
            const size_t signatureSize,
            const char *board,
            const char *currentVersion,
            Release &release) {
    if (size > kMaxManifestSize)
        return Status::TooLarge;
    if (!verifySignature(manifest, size, signature, signatureSize))
        return Status::BadSignature;
    return parse(manifest, size, board, currentVersion, release);
}

Status readSigned(const uint8_t *signedManifest,
                  const size_t size,
                  const char *board,
                  const char *currentVersion,
                  Release &release) {
    if (signedManifest == nullptr)
        return Status::BadSignature;
    if (size > kMaxSignedManifestSize)
        return Status::TooLarge;
    const uint8_t *newline = static_cast<const uint8_t *>(
        memchr(signedManifest,
               '\n',
               size < kMaxSignatureLineLength + 1 ? size : kMaxSignatureLineLength + 1));
    if (newline == nullptr)
        return Status::BadSignature;
    const size_t lineLength = static_cast<size_t>(newline - signedManifest);
    uint8_t signature[kMaxSignatureSize];
    size_t signatureSize = 0;
    if (lineLength == 0 || mbedtls_base64_decode(signature,
                                                 sizeof(signature),
                                                 &signatureSize,
                                                 signedManifest,
                                                 lineLength) != 0) {
        return Status::BadSignature;
    }
    return read(newline + 1,
                size - lineLength - 1,
                signature,
                signatureSize,
                board,
                currentVersion,
                release);
}

const char *statusName(const Status status) {
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::TooLarge:
        return "too_large";
    case Status::BadSignature:
        return "bad_signature";
    case Status::Malformed:
        return "malformed";
    case Status::UnsupportedFormat:
        return "unsupported_format";
    case Status::BoardMissing:
        return "board_missing";
    case Status::NotNewer:
        return "not_newer";
    }
    return "unknown";
}

} // namespace gateway::release
