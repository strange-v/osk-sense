#pragma once

#include "RadioCrypto.h"

namespace radiosensors {
namespace security {

constexpr uint8_t kProtocolMajor = 3;
constexpr uint8_t kActivationHeader = 0x6A;
constexpr uint8_t kAckHeader = 0x7F;
constexpr size_t kSaltSize = 8;
constexpr size_t kNodeTagSize = 8;
constexpr size_t kGatewayTagSize = 6;
constexpr size_t kMaxWireSize = 61;
constexpr size_t kMaxAckPayloadSize = 10;

enum class Direction : uint8_t { Node = 0, Gateway = 1 };
struct Transport { uint8_t target; uint8_t sender; uint8_t control; };
struct Keys { uint8_t encryption[16]; uint8_t authentication[16]; };
struct Context {
    osk::crypto::Aes128 encryption;
    osk::crypto::Cmac authentication;
};
static_assert(sizeof(Context) == 384, "radio crypto RAM budget");

void deriveKeys(const uint8_t factory[16], const uint8_t salt[kSaltSize],
                Keys& keys, osk::crypto::Cmac& scratch);
void initialize(Context& context, const Keys& keys);
// Reuses the authentication context during derivation.
void initialize(Context& context, const uint8_t factory[16],
                const uint8_t salt[kSaltSize]);
void counterBlock(Direction direction, uint8_t nodeId, uint8_t header,
                  uint32_t counter, uint8_t blockIndex, uint8_t output[16]);

// The clear prefix is the activation challenge (8 bytes), or empty.
// Failed authentication leaves the ciphertext untouched.
bool seal(const Context& context, Direction direction, uint8_t nodeId,
          Transport transport, uint32_t counter, uint8_t header,
          const uint8_t* payload, size_t payloadSize, uint8_t* output,
          size_t capacity, size_t& size, const uint8_t* challenge = nullptr);
bool open(const Context& context, Direction direction, uint8_t nodeId,
          Transport transport, uint8_t* wire, size_t size, uint32_t& counter,
          uint8_t*& payload, size_t& payloadSize);

struct Ack {
    bool commandPending = false;
    bool hasPowerTarget = false;
    uint8_t powerTarget = 0;
    bool hasCounterFloor = false;
    uint32_t counterFloor = 0;
    bool hasChallenge = false;
    uint8_t challenge[kSaltSize]{};
};
bool sealAck(const osk::crypto::Cmac& mac, Transport transport,
             uint32_t counter, const Ack& ack, uint8_t* output,
             size_t capacity, size_t& size);
bool openAck(const osk::crypto::Cmac& mac, Transport transport,
             uint32_t counter, const uint8_t* wire, size_t size, Ack& ack);

// Join confirm/complete bind the salt without transmitting it.
bool sealJoin(const osk::crypto::Cmac& mac, Transport transport,
              const uint8_t* frame, size_t frameSize, size_t tagSize,
              uint8_t* output, size_t capacity, size_t& size,
              const uint8_t* salt = nullptr);
bool verifyJoin(const osk::crypto::Cmac& mac, Transport transport,
                const uint8_t* wire, size_t size, size_t tagSize,
                const uint8_t* salt = nullptr);

}  // namespace security
}  // namespace radiosensors
