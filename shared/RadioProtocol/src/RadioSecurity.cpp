#include "RadioSecurity.h"

#include <string.h>

namespace radiosensors {
namespace security {
namespace {
void write32(uint8_t* output, uint32_t value) {
    for (uint8_t i = 0; i < 4; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t read32(const uint8_t* input) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(input[i]) << (8 * i);
    return value;
}
bool equalTag(const uint8_t* a, const uint8_t* b, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
bool headerValid(uint8_t header) {
    return (header >> 5) == kProtocolMajor && (header & 31) <= 10;
}
bool joinHeader(uint8_t header) {
    return header == 0x61 || header == 0x62 || header == 0x63 || header == 0x67;
}
bool nodeMatchesTransport(Direction direction, uint8_t nodeId, Transport transport) {
    return (direction == Direction::Node || direction == Direction::Gateway) &&
        nodeId != 0 && nodeId != 255 &&
        nodeId == (direction == Direction::Node ? transport.sender : transport.target);
}
size_t tagSize(Direction direction) {
    return direction == Direction::Node ? kNodeTagSize : kGatewayTagSize;
}
void computeTag(const osk::crypto::Cmac& mac, Transport transport,
                uint8_t header, const uint8_t* counter,
                const uint8_t* payload, size_t size, uint8_t tag[16],
                const uint8_t* salt = nullptr) {
    uint8_t input[4 + kMaxWireSize + kSaltSize];
    input[0] = header;
    input[1] = transport.target;
    input[2] = transport.sender;
    input[3] = transport.control;
    size_t position = 4;
    if (counter != nullptr) {
        memcpy(input + position, counter, 4);
        position += 4;
    }
    if (size != 0) memcpy(input + position, payload, size);
    position += size;
    if (salt != nullptr) {
        memcpy(input + position, salt, kSaltSize);
        position += kSaltSize;
    }
    osk::crypto::cmacCompute(mac, input, position, tag);
}
void crypt(const Context& context, Direction direction, uint8_t nodeId,
           uint8_t header, uint32_t counter, uint8_t* data, size_t size) {
    uint8_t block[16];
    for (uint8_t index = 0; size != 0; ++index) {
        counterBlock(direction, nodeId, header, counter, index, block);
        const uint8_t count = static_cast<uint8_t>(size > 16 ? 16 : size);
        osk::crypto::ctrApply(context.encryption, block, data, count);
        data += count;
        size -= count;
    }
}
}

void deriveKeys(const uint8_t factory[16], const uint8_t salt[kSaltSize],
                Keys& keys, osk::crypto::Cmac& scratch) {
    osk::crypto::cmacInit(scratch, factory);
    uint8_t input[1 + kSaltSize];
    memcpy(input + 1, salt, kSaltSize);
    input[0] = 1;
    osk::crypto::cmacCompute(scratch, input, sizeof(input), keys.encryption);
    input[0] = 2;
    osk::crypto::cmacCompute(scratch, input, sizeof(input), keys.authentication);
}
void initialize(Context& context, const Keys& keys) {
    osk::crypto::aesExpandKey(context.encryption, keys.encryption);
    osk::crypto::cmacInit(context.authentication, keys.authentication);
}
void initialize(Context& context, const uint8_t factory[16],
                const uint8_t salt[kSaltSize]) {
    Keys keys;
    deriveKeys(factory, salt, keys, context.authentication);
    initialize(context, keys);
    volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&keys);
    for (size_t i = 0; i < sizeof(keys); ++i) bytes[i] = 0;
}
void counterBlock(Direction direction, uint8_t nodeId, uint8_t header,
                  uint32_t counter, uint8_t blockIndex, uint8_t output[16]) {
    memset(output, 0, 16);
    output[0] = static_cast<uint8_t>(direction);
    output[1] = nodeId;
    output[2] = header;
    write32(output + 3, counter);
    output[15] = blockIndex;
}
bool seal(const Context& context, Direction direction, uint8_t nodeId,
          Transport transport, uint32_t counter, uint8_t header,
          const uint8_t* payload, size_t payloadSize, uint8_t* output,
          size_t capacity, size_t& size, const uint8_t* challenge) {
    const size_t clearSize = header == kActivationHeader ? kSaltSize : 0;
    const size_t overhead = 5 + clearSize + tagSize(direction);
    if (!nodeMatchesTransport(direction, nodeId, transport) ||
        !headerValid(header) || joinHeader(header) ||
        ((clearSize != 0) != (challenge != nullptr)) ||
        (clearSize != 0 && direction != Direction::Node) ||
        output == nullptr || (payloadSize != 0 && payload == nullptr) ||
        payloadSize > kMaxWireSize - overhead || capacity < payloadSize + overhead) return false;
    if (payloadSize != 0) memmove(output + 5 + clearSize, payload, payloadSize);
    output[0] = header;
    write32(output + 1, counter);
    if (clearSize != 0) memcpy(output + 5, challenge, clearSize);
    crypt(context, direction, nodeId, header, counter, output + 5 + clearSize, payloadSize);
    uint8_t tag[16];
    computeTag(context.authentication, transport, header, output + 1,
               output + 5, clearSize + payloadSize, tag);
    memcpy(output + 5 + clearSize + payloadSize, tag, tagSize(direction));
    size = overhead + payloadSize;
    return true;
}
bool open(const Context& context, Direction direction, uint8_t nodeId,
          Transport transport, uint8_t* wire, size_t size, uint32_t& counter,
          uint8_t*& payload, size_t& payloadSize) {
    if (!nodeMatchesTransport(direction, nodeId, transport) ||
        wire == nullptr || size < 5 + tagSize(direction) || size > kMaxWireSize ||
        !headerValid(wire[0]) || joinHeader(wire[0])) return false;
    const size_t clearSize = wire[0] == kActivationHeader ? kSaltSize : 0;
    if (size < 5 + clearSize + tagSize(direction) ||
        (clearSize != 0 && direction != Direction::Node)) return false;
    uint8_t tag[16];
    computeTag(context.authentication, transport, wire[0], wire + 1,
               wire + 5, size - 5 - tagSize(direction), tag);
    if (!equalTag(tag, wire + size - tagSize(direction), tagSize(direction))) return false;
    counter = read32(wire + 1);
    payload = wire + 5 + clearSize;
    payloadSize = size - 5 - clearSize - tagSize(direction);
    crypt(context, direction, nodeId, wire[0], counter, payload, payloadSize);
    return true;
}
bool sealAck(const osk::crypto::Cmac& mac, Transport transport,
             uint32_t counter, const Ack& ack, uint8_t* output,
             size_t capacity, size_t& size) {
    const size_t payloadSize = 1 + (ack.hasPowerTarget ? 1 : 0) +
        (ack.hasCounterFloor ? 4 : 0) + (ack.hasChallenge ? kSaltSize : 0);
    if (output == nullptr || capacity < payloadSize + kGatewayTagSize ||
        (ack.hasCounterFloor && ack.hasChallenge) ||
        (ack.hasPowerTarget && ack.powerTarget > 31)) return false;
    output[0] = static_cast<uint8_t>(ack.commandPending | (ack.hasPowerTarget << 1) |
        (ack.hasCounterFloor << 2) | (ack.hasChallenge << 3));
    size_t position = 1;
    if (ack.hasPowerTarget) output[position++] = ack.powerTarget;
    if (ack.hasCounterFloor) { write32(output + position, ack.counterFloor); position += 4; }
    if (ack.hasChallenge) { memcpy(output + position, ack.challenge, kSaltSize); position += kSaltSize; }
    uint8_t counterBytes[4], tag[16];
    write32(counterBytes, counter);
    computeTag(mac, transport, kAckHeader, counterBytes, output, position, tag);
    memcpy(output + position, tag, kGatewayTagSize);
    size = position + kGatewayTagSize;
    return true;
}
bool openAck(const osk::crypto::Cmac& mac, Transport transport,
             uint32_t counter, const uint8_t* wire, size_t size, Ack& ack) {
    if (wire == nullptr || size < kGatewayTagSize ||
        size > kMaxAckPayloadSize + kGatewayTagSize) return false;
    const size_t payloadSize = size - kGatewayTagSize;
    uint8_t counterBytes[4], tag[16];
    write32(counterBytes, counter);
    computeTag(mac, transport, kAckHeader, counterBytes, wire, payloadSize, tag);
    if (!equalTag(tag, wire + payloadSize, kGatewayTagSize)) return false;
    Ack result{};
    if (payloadSize != 0) {
        const uint8_t flags = wire[0];
        if ((flags & 0xF0) != 0 || (flags & 0x0C) == 0x0C) return false;
        result.commandPending = (flags & 1) != 0;
        result.hasPowerTarget = (flags & 2) != 0;
        result.hasCounterFloor = (flags & 4) != 0;
        result.hasChallenge = (flags & 8) != 0;
        const size_t expected = 1 + (result.hasPowerTarget ? 1 : 0) +
            (result.hasCounterFloor ? 4 : 0) + (result.hasChallenge ? kSaltSize : 0);
        if (payloadSize != expected) return false;
        size_t position = 1;
        if (result.hasPowerTarget) { result.powerTarget = wire[position++]; if (result.powerTarget > 31) return false; }
        if (result.hasCounterFloor) { result.counterFloor = read32(wire + position); position += 4; }
        if (result.hasChallenge) memcpy(result.challenge, wire + position, kSaltSize);
    }
    ack = result;
    return true;
}
bool sealJoin(const osk::crypto::Cmac& mac, Transport transport,
              const uint8_t* frame, size_t frameSize, size_t length,
              uint8_t* output, size_t capacity, size_t& size, const uint8_t* salt) {
    if (frame == nullptr || output == nullptr || frameSize == 0 ||
        !joinHeader(frame[0]) || (length != kNodeTagSize && length != kGatewayTagSize) ||
        frameSize > kMaxWireSize - length || capacity < frameSize + length) return false;
    uint8_t tag[16];
    computeTag(mac, transport, frame[0], nullptr, frame + 1, frameSize - 1, tag, salt);
    memmove(output, frame, frameSize);
    memcpy(output + frameSize, tag, length);
    size = frameSize + length;
    return true;
}
bool verifyJoin(const osk::crypto::Cmac& mac, Transport transport,
                const uint8_t* wire, size_t size, size_t length, const uint8_t* salt) {
    if (wire == nullptr || (length != kNodeTagSize && length != kGatewayTagSize) ||
        size <= length || size > kMaxWireSize || !joinHeader(wire[0])) return false;
    uint8_t tag[16];
    computeTag(mac, transport, wire[0], nullptr, wire + 1, size - length - 1, tag, salt);
    return equalTag(tag, wire + size - length, length);
}
}  // namespace security
}  // namespace radiosensors
