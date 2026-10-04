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

}  // namespace crypto
}  // namespace osk
