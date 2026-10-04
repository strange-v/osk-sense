#include "OskCrypto.h"

#include <string.h>

namespace osk {
namespace crypto {
namespace {

struct SBox {
    uint8_t value[256];
};

constexpr uint8_t rotl8(const uint8_t x, const uint8_t shift) {
    return static_cast<uint8_t>((x << shift) | (x >> (8 - shift)));
}

// Generated at compile time so the table cannot carry a transcription error.
constexpr SBox makeSBox() {
    SBox box{};
    uint8_t p = 1;
    uint8_t q = 1;
    do {
        p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q = static_cast<uint8_t>(q ^ (q << 1));
        q = static_cast<uint8_t>(q ^ (q << 2));
        q = static_cast<uint8_t>(q ^ (q << 4));
        if (q & 0x80) q = static_cast<uint8_t>(q ^ 0x09);
        box.value[p] = static_cast<uint8_t>(
            q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    box.value[0] = 0x63;
    return box;
}

constexpr SBox kSBox = makeSBox();

// Bench variants: OSK_SBOX_RAM looks the table up in SRAM instead of mapped
// flash; OSK_AES_O2 compiles the round functions for speed.
#if defined(OSK_SBOX_RAM)
SBox ramSBox;
bool ramSBoxReady = false;
#define OSK_SBOX(x) ramSBox.value[(x)]
#else
#define OSK_SBOX(x) kSBox.value[(x)]
#endif

#if defined(OSK_AES_O2)
#define OSK_HOT __attribute__((optimize("O2")))
#else
#define OSK_HOT
#endif

inline uint8_t xtime(const uint8_t x) {
    // Shifting a uint8_t copy keeps AVR code in one register.
    uint8_t doubled = x;
    doubled <<= 1;
    if (x & 0x80) doubled ^= 0x1B;
    return doubled;
}

OSK_HOT void xorBlock(uint8_t* target, const uint8_t* source) {
    for (uint8_t i = 0; i < kBlockSize; ++i) target[i] ^= source[i];
}

#if !defined(OSK_AES_FUSED)
OSK_HOT void subShift(uint8_t* s) {
    for (uint8_t i = 0; i < kBlockSize; ++i) s[i] = OSK_SBOX(s[i]);
    uint8_t t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;
}

OSK_HOT void mixColumns(uint8_t* s) {
    for (uint8_t c = 0; c < kBlockSize; c += 4) {
        const uint8_t a0 = s[c];
        const uint8_t a1 = s[c + 1];
        const uint8_t a2 = s[c + 2];
        const uint8_t a3 = s[c + 3];
        const uint8_t all = a0 ^ a1 ^ a2 ^ a3;
        s[c] = a0 ^ all ^ xtime(a0 ^ a1);
        s[c + 1] = a1 ^ all ^ xtime(a1 ^ a2);
        s[c + 2] = a2 ^ all ^ xtime(a2 ^ a3);
        s[c + 3] = a3 ^ all ^ xtime(a3 ^ a0);
    }
}
#endif

void doubleBlock(uint8_t* out, const uint8_t* in) {
    const uint8_t carry = (in[0] & 0x80) ? 0x87 : 0;
    for (uint8_t i = 0; i < kBlockSize - 1; ++i) {
        out[i] = static_cast<uint8_t>((in[i] << 1) | (in[i + 1] >> 7));
    }
    out[kBlockSize - 1] = static_cast<uint8_t>((in[kBlockSize - 1] << 1) ^ carry);
}

}  // namespace

void aesExpandKey(Aes128& aes, const uint8_t key[kBlockSize]) {
#if defined(OSK_SBOX_RAM)
    if (!ramSBoxReady) {
        ramSBox = kSBox;
        ramSBoxReady = true;
    }
#endif
    uint8_t* rk = aes.roundKeys;
    memcpy(rk, key, kBlockSize);
    uint8_t rcon = 1;
    for (uint8_t i = kBlockSize; i < sizeof(aes.roundKeys); i += 4) {
        uint8_t t0 = rk[i - 4];
        uint8_t t1 = rk[i - 3];
        uint8_t t2 = rk[i - 2];
        uint8_t t3 = rk[i - 1];
        if ((i & 15) == 0) {
            const uint8_t first = t0;
            t0 = OSK_SBOX(t1) ^ rcon;
            t1 = OSK_SBOX(t2);
            t2 = OSK_SBOX(t3);
            t3 = OSK_SBOX(first);
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ t0;
        rk[i + 1] = rk[i - 15] ^ t1;
        rk[i + 2] = rk[i - 14] ^ t2;
        rk[i + 3] = rk[i - 13] ^ t3;
    }
}

#if defined(OSK_AES_FUSED)
// One pass per round: each output column reads its four shifted bytes
// through the S-box, mixes them, and adds the round key, so the state is
// loaded and stored once per round instead of once per step.
OSK_HOT void aesEncrypt(const Aes128& aes, uint8_t block[kBlockSize]) {
    const uint8_t* rk = aes.roundKeys;
    uint8_t first[kBlockSize];
    uint8_t second[kBlockSize];
    uint8_t* s = first;
    uint8_t* t = second;
    for (uint8_t i = 0; i < kBlockSize; ++i) s[i] = block[i] ^ rk[i];
    for (uint8_t round = 1; round < 10; ++round) {
        rk += kBlockSize;
#if defined(OSK_AES_UNROLL)
#define OSK_COLUMN(c)                                              \
    do {                                                           \
        const uint8_t a0 = OSK_SBOX(s[(c)]);                       \
        const uint8_t a1 = OSK_SBOX(s[((c) + 5) & 15]);            \
        const uint8_t a2 = OSK_SBOX(s[((c) + 10) & 15]);           \
        const uint8_t a3 = OSK_SBOX(s[((c) + 15) & 15]);           \
        const uint8_t all = a0 ^ a1 ^ a2 ^ a3;                     \
        t[(c)] = a0 ^ all ^ xtime(a0 ^ a1) ^ rk[(c)];              \
        t[(c) + 1] = a1 ^ all ^ xtime(a1 ^ a2) ^ rk[(c) + 1];      \
        t[(c) + 2] = a2 ^ all ^ xtime(a2 ^ a3) ^ rk[(c) + 2];      \
        t[(c) + 3] = a3 ^ all ^ xtime(a3 ^ a0) ^ rk[(c) + 3];      \
    } while (0)
        OSK_COLUMN(0);
        OSK_COLUMN(4);
        OSK_COLUMN(8);
        OSK_COLUMN(12);
#undef OSK_COLUMN
#else
        for (uint8_t c = 0; c < kBlockSize; c += 4) {
            const uint8_t a0 = OSK_SBOX(s[c]);
            const uint8_t a1 = OSK_SBOX(s[(c + 5) & 15]);
            const uint8_t a2 = OSK_SBOX(s[(c + 10) & 15]);
            const uint8_t a3 = OSK_SBOX(s[(c + 15) & 15]);
            const uint8_t all = a0 ^ a1 ^ a2 ^ a3;
            t[c] = a0 ^ all ^ xtime(a0 ^ a1) ^ rk[c];
            t[c + 1] = a1 ^ all ^ xtime(a1 ^ a2) ^ rk[c + 1];
            t[c + 2] = a2 ^ all ^ xtime(a2 ^ a3) ^ rk[c + 2];
            t[c + 3] = a3 ^ all ^ xtime(a3 ^ a0) ^ rk[c + 3];
        }
#endif
        uint8_t* swap = s;
        s = t;
        t = swap;
    }
    rk += kBlockSize;
    for (uint8_t i = 0; i < kBlockSize; ++i) {
        block[i] = OSK_SBOX(s[(i * 5) & 15]) ^ rk[i];
    }
}
#else
OSK_HOT void aesEncrypt(const Aes128& aes, uint8_t block[kBlockSize]) {
    const uint8_t* rk = aes.roundKeys;
    xorBlock(block, rk);
    for (uint8_t round = 1; round < 10; ++round) {
        subShift(block);
        mixColumns(block);
        rk += kBlockSize;
        xorBlock(block, rk);
    }
    subShift(block);
    xorBlock(block, rk + kBlockSize);
}
#endif

void cmacInit(Cmac& cmac, const uint8_t key[kBlockSize]) {
    aesExpandKey(cmac.aes, key);
    uint8_t l[kBlockSize]{};
    aesEncrypt(cmac.aes, l);
    doubleBlock(cmac.k1, l);
    doubleBlock(cmac.k2, cmac.k1);
}

void cmacCompute(
    const Cmac& cmac, const uint8_t* message, size_t length,
    uint8_t tag[kBlockSize]) {
    memset(tag, 0, kBlockSize);
    while (length > kBlockSize) {
        xorBlock(tag, message);
        aesEncrypt(cmac.aes, tag);
        message += kBlockSize;
        length -= kBlockSize;
    }
    const uint8_t* subkey = cmac.k1;
    if (length < kBlockSize) {
        tag[length] ^= 0x80;
        subkey = cmac.k2;
    }
    for (uint8_t i = 0; i < length; ++i) tag[i] ^= message[i];
    xorBlock(tag, subkey);
    aesEncrypt(cmac.aes, tag);
}

void ctrApply(
    const Aes128& aes, const uint8_t counterBlock[kBlockSize], uint8_t* data,
    const uint8_t length) {
    uint8_t stream[kBlockSize];
    memcpy(stream, counterBlock, kBlockSize);
    aesEncrypt(aes, stream);
    for (uint8_t i = 0; i < length && i < kBlockSize; ++i) data[i] ^= stream[i];
}

namespace {

// Absorbs bytes into a CBC-MAC state; returns the new fill position. Zero
// padding of a final partial block is implicit: unfilled bytes stay as XORed.
uint8_t cbcAbsorb(
    const Aes128& aes, uint8_t* state, uint8_t position, const uint8_t* bytes,
    uint8_t length) {
    for (uint8_t i = 0; i < length; ++i) {
        state[position++] ^= bytes[i];
        if (position == kBlockSize) {
            aesEncrypt(aes, state);
            position = 0;
        }
    }
    return position;
}

}  // namespace

void ccmSeal(
    const Aes128& aes, const uint8_t* nonce, const uint8_t nonceLength,
    const uint8_t* aad, const uint8_t aadLength, uint8_t* data,
    const uint8_t dataLength, uint8_t* tag, const uint8_t tagLength) {
    const uint8_t lengthSize = static_cast<uint8_t>(15 - nonceLength);
    uint8_t mac[kBlockSize]{};
    mac[0] = static_cast<uint8_t>(
        (aadLength ? 0x40 : 0) | (((tagLength - 2) / 2) << 3) |
        (lengthSize - 1));
    memcpy(mac + 1, nonce, nonceLength);
    mac[kBlockSize - 1] = dataLength;
    aesEncrypt(aes, mac);
    if (aadLength != 0) {
        mac[1] ^= aadLength;
        uint8_t position = cbcAbsorb(aes, mac, 2, aad, aadLength);
        if (position != 0) aesEncrypt(aes, mac);
    }
    uint8_t position = cbcAbsorb(aes, mac, 0, data, dataLength);
    if (position != 0) aesEncrypt(aes, mac);

    uint8_t counter[kBlockSize]{};
    counter[0] = static_cast<uint8_t>(lengthSize - 1);
    memcpy(counter + 1, nonce, nonceLength);
    uint8_t stream[kBlockSize];
    for (uint8_t offset = 0, index = 1; offset < dataLength;
         offset += kBlockSize, ++index) {
        counter[kBlockSize - 1] = index;
        memcpy(stream, counter, kBlockSize);
        aesEncrypt(aes, stream);
        for (uint8_t i = 0; i < kBlockSize && offset + i < dataLength; ++i) {
            data[offset + i] ^= stream[i];
        }
    }
    counter[kBlockSize - 1] = 0;
    memcpy(stream, counter, kBlockSize);
    aesEncrypt(aes, stream);
    for (uint8_t i = 0; i < tagLength; ++i) tag[i] = mac[i] ^ stream[i];
}

}  // namespace crypto
}  // namespace osk
