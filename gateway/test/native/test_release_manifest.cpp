#include "ReleaseManifest.h"

#include <mbedtls/base64.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace gateway::release;
using Bytes = std::vector<uint8_t>;

// manifest.json and manifest.sig exactly as published by release 0.9.0.
const std::string kManifest =
    R"({"format":1,"version":"0.9.0","boards":{"wt32-eth01":{"firmware":{"file":"gateway-wt32-eth01-firmware.bin","size":843920,"sha256":"fc669953090780fd28073154c2982608c2936a95151037aad97fd5853bc6665e"},"ui":{"file":"gateway-wt32-eth01-ui.bin","size":1409024,"sha256":"58b2176c51b1a0e757f9b38e4cc10ce7be9188241653bb7e4bc382a7720c5357"}},"waveshare-s3-eth":{"firmware":{"file":"gateway-waveshare-s3-eth-firmware.bin","size":878816,"sha256":"ba5d0ce5e26d7eb858f3e95768c4dc56882a993392045f38e5914f02cc545299"},"ui":{"file":"gateway-waveshare-s3-eth-ui.bin","size":10321920,"sha256":"a4d31ff703fda8de22c7f9f60debebb60293791af10e60facb0c7be4b15c0dac"}}}})"
    "\n";
const char kSignatureHex[] =
    "3045022044cb4479f623ff28fde401a3f420ff7209ab25c23f31e2b0fc63030613f960e0"
    "022100f4c3d694a07f62fd9642fc3437b5fcce049eb7366158f2164495a5ae992f71de";

Bytes fromHex(const char *hex) {
    Bytes bytes;
    for (size_t index = 0; hex[index] != '\0'; index += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + index, 2), nullptr, 16)));
    return bytes;
}

std::string toHex(const uint8_t *bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (size_t index = 0; index < size; ++index) {
        hex += digits[bytes[index] >> 4];
        hex += digits[bytes[index] & 0x0f];
    }
    return hex;
}

const uint8_t *data(const std::string &text) {
    return reinterpret_cast<const uint8_t *>(text.data());
}

Status readManifest(const std::string &manifest,
                    const Bytes &signature,
                    const char *board = "waveshare-s3-eth",
                    const char *current = "0.8.0") {
    Release release;
    return read(data(manifest),
                manifest.size(),
                signature.data(),
                signature.size(),
                board,
                current,
                release);
}

Status parseJson(const std::string &json, const char *board = "b", const char *current = "0.8.0") {
    Release release;
    return parse(data(json), json.size(), board, current, release);
}

const std::string kUi =
    R"({"file":"u.bin.gz","size":1,"image_size":2,"sha256":")" + std::string(64, 'a') + R"("})";

std::string manifestWith(const std::string &firmware, const std::string &ui = kUi) {
    return R"({"format":2,"version":"1.0.0","boards":{"b":{"firmware":)" + firmware + R"(,"ui":)" +
           ui + R"(}}})";
}

std::string image(const std::string &file, const std::string &size, const std::string &sha256) {
    return R"({"file":")" + file + R"(","size":)" + size + R"(,"sha256":")" + sha256 + R"("})";
}

void publicKeyMatchesFile(const char *path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    std::stringstream contents;
    contents << file.rdbuf();
    std::string pem = contents.str();
    pem.erase(std::remove(pem.begin(), pem.end(), '\r'), pem.end());
    assert(pem == kSigningPublicKey);
}

void versions() {
    Version v;
    assert(parseVersion("0.9.0", v) && v.major == 0 && v.minor == 9 && v.patch == 0);
    assert(parseVersion("65535.65535.65535", v) && v.patch == 65535);
    for (const char *bad : {"",
                            "1",
                            "1.2",
                            "1.2.3.4",
                            "01.2.3",
                            "1.02.3",
                            "1.2.03",
                            "1.2.3-rc1",
                            "v1.2.3",
                            " 1.2.3",
                            "1.2.3 ",
                            "1..3",
                            "65536.0.0",
                            "-1.0.0"}) {
        assert(!parseVersion(bad, v));
    }
    assert(!parseVersion(nullptr, v));
    Version a, b;
    parseVersion("0.9.10", a);
    parseVersion("0.9.9", b);
    assert(compare(a, b) > 0 && compare(b, a) < 0 && compare(a, a) == 0);
    parseVersion("1.0.0", a);
    parseVersion("0.99.99", b);
    assert(compare(a, b) > 0);
    std::cout << "Release versions: parsing and ordering passed\n";
}

void publishedRelease() {
    const Bytes signature = fromHex(kSignatureHex);
    assert(verifySignature(data(kManifest), kManifest.size(), signature.data(), signature.size()));

    // 0.9.0 predates manifest format 2: the signature passes, the format does not.
    assert(readManifest(kManifest, signature, "wt32-eth01") == Status::UnsupportedFormat);

    // The same release in format 2, unsigned, through the parser read() uses.
    std::string current = kManifest;
    const auto replace = [&current](const std::string &from, const std::string &to) {
        const size_t at = current.find(from);
        assert(at != std::string::npos);
        current.replace(at, from.size(), to);
    };
    replace(R"("format":1)", R"("format":2)");
    replace(R"("gateway-wt32-eth01-ui.bin","size":1409024,)",
            R"("gateway-wt32-eth01-ui.bin.gz","size":154040,"image_size":1409024,)");
    replace(R"("gateway-waveshare-s3-eth-ui.bin","size":10321920,)",
            R"("gateway-waveshare-s3-eth-ui.bin.gz","size":163187,"image_size":10321920,)");
    Release release;
    assert(parse(::data(current), current.size(), "wt32-eth01", "0.8.5", release) == Status::Ok);
    assert(std::strcmp(release.versionText, "0.9.0") == 0);
    assert(std::strcmp(release.firmware.file, "gateway-wt32-eth01-firmware.bin") == 0);
    assert(release.firmware.size == 843920 && release.firmware.imageSize == 843920);
    assert(toHex(release.firmware.sha256, kSha256Size) ==
           "fc669953090780fd28073154c2982608c2936a95151037aad97fd5853bc6665e");
    assert(std::strcmp(release.ui.file, "gateway-wt32-eth01-ui.bin.gz") == 0);
    assert(release.ui.size == 154040 && release.ui.imageSize == 1409024);
    assert(toHex(release.ui.sha256, kSha256Size) ==
           "58b2176c51b1a0e757f9b38e4cc10ce7be9188241653bb7e4bc382a7720c5357");

    assert(parseJson(current, "waveshare-s3-eth", "0.9.0") == Status::NotNewer);
    assert(parseJson(current, "waveshare-s3-eth", "1.0.0") == Status::NotNewer);
    assert(parseJson(current, "esp32-other") == Status::BoardMissing);
    std::cout << "Release manifest: published 0.9.0 signature verified; format 2 read\n";
}

void tampering() {
    const Bytes signature = fromHex(kSignatureHex);
    for (size_t index = 0; index < kManifest.size(); ++index) {
        std::string changed = kManifest;
        changed[index] ^= 0x01;
        assert(readManifest(changed, signature) == Status::BadSignature);
    }
    assert(readManifest(kManifest.substr(0, kManifest.size() - 1), signature) ==
           Status::BadSignature);
    assert(readManifest(kManifest + " ", signature) == Status::BadSignature);
    for (size_t index = 0; index < signature.size(); ++index) {
        Bytes changed = signature;
        changed[index] ^= 0x01;
        assert(readManifest(kManifest, changed) == Status::BadSignature);
    }
    assert(readManifest(kManifest, Bytes(signature.begin(), signature.end() - 1)) ==
           Status::BadSignature);
    assert(readManifest(kManifest, Bytes()) == Status::BadSignature);
    assert(readManifest(kManifest, Bytes(kMaxSignatureSize + 1, 0x30)) == Status::BadSignature);
    assert(readManifest(std::string(kMaxManifestSize + 1, ' '), signature) == Status::TooLarge);
    std::cout << "Release manifest: every flipped manifest and signature byte rejected\n";
}

std::string base64(const Bytes &bytes) {
    unsigned char output[200];
    size_t written = 0;
    assert(mbedtls_base64_encode(output, sizeof(output), &written, bytes.data(), bytes.size()) ==
           0);
    return std::string(reinterpret_cast<char *>(output), written);
}

Status readEnvelope(const std::string &envelope, const char *current = "0.8.0") {
    Release release;
    return readSigned(data(envelope), envelope.size(), "wt32-eth01", current, release);
}

void signedEnvelope() {
    const Bytes signature = fromHex(kSignatureHex);
    const std::string line = base64(signature);
    assert(line.size() <= kMaxSignatureLineLength);
    const std::string envelope = line + "\n" + kManifest;

    // Reaching the format check means the envelope split and the signature passed.
    assert(readEnvelope(envelope) == Status::UnsupportedFormat);

    assert(readEnvelope(kManifest) == Status::BadSignature);
    assert(readEnvelope("\n" + kManifest) == Status::BadSignature);
    assert(readEnvelope(line + "\r\n" + kManifest) == Status::BadSignature);
    assert(readEnvelope(line + "\n\n" + kManifest) == Status::BadSignature);
    assert(readEnvelope("!" + line.substr(1) + "\n" + kManifest) == Status::BadSignature);
    assert(readEnvelope(line) == Status::BadSignature);
    assert(readEnvelope(line + "\n") == Status::BadSignature);
    assert(readEnvelope(std::string(kMaxSignatureLineLength + 1, 'A') + "\n" + kManifest) ==
           Status::BadSignature);
    std::string changed = envelope;
    changed[line.size() + 10] ^= 0x01;
    assert(readEnvelope(changed) == Status::BadSignature);
    assert(readEnvelope(line + "\n" + std::string(kMaxManifestSize + 1, ' ')) == Status::TooLarge);
    std::cout << "Release manifest: signed envelope split, verified and tampering rejected\n";
}

void malformed() {
    const std::string sha(64, 'a');
    assert(parseJson(manifestWith(image("f.bin", "1", sha))) == Status::Ok);
    assert(parseJson(manifestWith(image("f.bin", "1", sha)), "b", "1.0.0") == Status::NotNewer);
    assert(parseJson(R"({"format":1,"version":"1.0.0","boards":{}})") == Status::UnsupportedFormat);
    assert(parseJson(R"({"format":3,"version":"1.0.0","boards":{}})") == Status::UnsupportedFormat);
    for (const std::string &json : std::vector<std::string>{
             "",
             "[]",
             "{",
             R"({"version":"1.0.0","boards":{}})",
             R"({"format":"1","version":"1.0.0","boards":{}})",
             R"({"format":2,"version":"1.0","boards":{}})",
             R"({"format":2,"version":"1.0.0","boards":[]})",
             R"({"format":2,"version":"1.0.0","boards":{"b":"x"}})",
             R"({"format":2,"version":"1.0.0","boards":{"b":{"firmware":{}}}})",
             manifestWith(image("f.bin", "1", sha),
                          R"({"file":"u.gz","size":1,"sha256":")" + sha + R"("})"),
             manifestWith(image("f.bin", "1", sha),
                          R"({"file":"u.gz","size":1,"image_size":0,"sha256":")" + sha + R"("})"),
             manifestWith(image("f.bin", "1", sha),
                          R"({"file":"u.gz","size":1,"image_size":"2","sha256":")" + sha + R"("})"),
             manifestWith(R"({"file":"f.bin","size":1,"image_size":1,"sha256":")" + sha + R"("})"),
             manifestWith(image("", "1", sha)),
             manifestWith(image("../f.bin", "1", sha)),
             manifestWith(image("dir/f.bin", "1", sha)),
             manifestWith(image(".hidden", "1", sha)),
             manifestWith(image("f bin", "1", sha)),
             manifestWith(image("f%2F.bin", "1", sha)),
             manifestWith(image(std::string(kMaxFileNameLength + 1, 'f'), "1", sha)),
             manifestWith(image("f.bin", "0", sha)),
             manifestWith(image("f.bin", "-1", sha)),
             manifestWith(image("f.bin", "4294967296", sha)),
             manifestWith(image("f.bin", "\"1\"", sha)),
             manifestWith(image("f.bin", "1", sha.substr(1))),
             manifestWith(image("f.bin", "1", sha + "a")),
             manifestWith(image("f.bin", "1", std::string(64, 'A'))),
             manifestWith(image("f.bin", "1", std::string(63, 'a') + "g")),
             manifestWith(R"({"file":"f.bin","size":1,"sha256":")" + sha + R"(","extra":1})"),
         }) {
        assert(parseJson(json) == Status::Malformed);
    }
    assert(parseJson(manifestWith(image(std::string(kMaxFileNameLength, 'f'), "1", sha))) ==
           Status::Ok);
    assert(parseJson(R"({"format":2,"version":"1.0.0","boards":{}})") == Status::BoardMissing);
    assert(parseJson(manifestWith(image("f.bin", "1", sha)), "b", "bad") == Status::Malformed);
    std::cout << "Release manifest: malformed fields, unsafe file names and bad hashes rejected\n";
}

int main(int argc, char **argv) {
    assert(argc > 1); // Path to gateway-signing.pub.pem.
    publicKeyMatchesFile(argv[1]);
    versions();
    publishedRelease();
    tampering();
    signedEnvelope();
    malformed();
}
