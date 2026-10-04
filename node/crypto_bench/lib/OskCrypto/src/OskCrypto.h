#pragma once

#include <stddef.h>
#include <stdint.h>

namespace osk {
namespace crypto {

constexpr uint8_t kBlockSize = 16;

// Expanded AES-128 encryption key. Only the forward cipher is implemented:
// CTR and CMAC never decrypt.
struct Aes128 {
    uint8_t roundKeys[176];
};

void aesExpandKey(Aes128& aes, const uint8_t key[kBlockSize]);
void aesEncrypt(const Aes128& aes, uint8_t block[kBlockSize]);

struct Cmac {
    Aes128 aes;
    uint8_t k1[kBlockSize];
    uint8_t k2[kBlockSize];
};

void cmacInit(Cmac& cmac, const uint8_t key[kBlockSize]);
// Full 16-byte tag; callers truncate.
void cmacCompute(
    const Cmac& cmac, const uint8_t* message, size_t length,
    uint8_t tag[kBlockSize]);

// XORs `data` with the keystream starting at `counterBlock`; at most one
// block, which every OSK frame fits.
void ctrApply(
    const Aes128& aes, const uint8_t counterBlock[kBlockSize], uint8_t* data,
    uint8_t length);

// AES-CCM (SP 800-38C, RFC 3610): encrypts `data` in place and writes a
// `tagLength`-byte tag. nonceLength is 7..13, tagLength 4..16 and even,
// aadLength below 0xFF00, dataLength below 256.
void ccmSeal(
    const Aes128& aes, const uint8_t* nonce, uint8_t nonceLength,
    const uint8_t* aad, uint8_t aadLength, uint8_t* data, uint8_t dataLength,
    uint8_t* tag, uint8_t tagLength);

}  // namespace crypto
}  // namespace osk
