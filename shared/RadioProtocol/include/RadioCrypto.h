#pragma once

#include <stddef.h>
#include <stdint.h>

namespace osk {
namespace crypto {

constexpr uint8_t kBlockSize = 16;
struct Aes128 { uint8_t roundKeys[176]; };
struct Cmac {
    Aes128 aes;
    uint8_t k1[kBlockSize];
    uint8_t k2[kBlockSize];
};

void aesExpandKey(Aes128& aes, const uint8_t key[kBlockSize]);
void aesEncrypt(const Aes128& aes, uint8_t block[kBlockSize]);
void cmacInit(Cmac& cmac, const uint8_t key[kBlockSize]);
void cmacCompute(const Cmac& cmac, const uint8_t* message, size_t length,
                 uint8_t tag[kBlockSize]);
// Applies at most one AES block. Frame formatting supplies the block index.
void ctrApply(const Aes128& aes, const uint8_t counterBlock[kBlockSize],
              uint8_t* data, uint8_t length);

}  // namespace crypto
}  // namespace osk
