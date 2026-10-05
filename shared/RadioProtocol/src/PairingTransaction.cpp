#include "PairingTransaction.h"

#include <string.h>

namespace radiosensors {
namespace security {
namespace pairing {
namespace {
constexpr uint8_t kMagic[] = {'R','S','P','T'};
constexpr size_t kRequestOffset = 19, kAcceptOffset = 52, kKeysOffset = 87, kCrcOffset = 119;
static_assert(kRequestOffset + frames::kJoinRequestSize == kAcceptOffset, "request layout");
static_assert(kAcceptOffset + frames::kJoinAcceptSize == kKeysOffset, "accept layout");
static_assert(kKeysOffset + 32 == kCrcOffset && kCrcOffset + 4 == kSnapshotSize, "keys layout");
uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return ~crc;
}
bool same(Transport a, Transport b) {
    return a.target == b.target && a.sender == b.sender && a.control == b.control;
}
void writeTransport(uint8_t* out, Transport value) {
    out[0] = value.target; out[1] = value.sender; out[2] = value.control;
}
bool valid(const Record& value) {
    if (value.state == State::Empty) {
        const Record empty;
        return same(value.requestTransport, empty.requestTransport) &&
            same(value.acceptTransport, empty.acceptTransport) &&
            memcmp(value.request, empty.request, sizeof(value.request)) == 0 &&
            memcmp(value.accept, empty.accept, sizeof(value.accept)) == 0 &&
            memcmp(value.keys.encryption, empty.keys.encryption, 16) == 0 &&
            memcmp(value.keys.authentication, empty.keys.authentication, 16) == 0;
    }
    return (value.state == State::Pending || value.state == State::Active) &&
        value.requestTransport.sender == 0 && value.requestTransport.target == 100 &&
        value.acceptTransport.sender == 100 && value.acceptTransport.target == 0 &&
        value.request[0] == frames::kJoinRequestHeader &&
        value.accept[0] == frames::kJoinAcceptHeader &&
        memcmp(value.request + 1, value.accept + 1, 18) == 0 &&
        protocol::readUint16Le(value.request + 19) != 0 && value.request[24] <= 31 &&
        value.accept[27] >= 1 && value.accept[27] <= 99 && value.accept[28] != 0;
}
}

bool encode(const Record& record, uint32_t generation, uint8_t* output, size_t size) {
    if (!output || size != kSnapshotSize || !valid(record)) return false;
    memcpy(output, kMagic, 4);
    protocol::writeUint16Le(output + 4, 3);
    protocol::writeUint32Le(output + 6, generation);
    protocol::writeUint16Le(output + 10, kSnapshotSize);
    output[12] = static_cast<uint8_t>(record.state);
    writeTransport(output + 13, record.requestTransport);
    writeTransport(output + 16, record.acceptTransport);
    memcpy(output + kRequestOffset, record.request, sizeof(record.request));
    memcpy(output + kAcceptOffset, record.accept, sizeof(record.accept));
    memcpy(output + kKeysOffset, record.keys.encryption, 16);
    memcpy(output + kKeysOffset + 16, record.keys.authentication, 16);
    protocol::writeUint32Le(output + kCrcOffset, crc32(output, kCrcOffset));
    return true;
}

bool decode(const uint8_t* data, size_t size, Record& record, uint32_t& generation) {
    if (!data || size != kSnapshotSize || memcmp(data, kMagic, 4) != 0 ||
        protocol::readUint16Le(data + 4) != 3 ||
        protocol::readUint16Le(data + 10) != kSnapshotSize ||
        protocol::readUint32Le(data + kCrcOffset) != crc32(data, kCrcOffset)) return false;
    Record candidate;
    candidate.state = static_cast<State>(data[12]);
    candidate.requestTransport = Transport{data[13],data[14],data[15]};
    candidate.acceptTransport = Transport{data[16],data[17],data[18]};
    memcpy(candidate.request, data + kRequestOffset, sizeof(candidate.request));
    memcpy(candidate.accept, data + kAcceptOffset, sizeof(candidate.accept));
    memcpy(candidate.keys.encryption, data + kKeysOffset, 16);
    memcpy(candidate.keys.authentication, data + kKeysOffset + 16, 16);
    if (!valid(candidate)) return false;
    record = candidate;
    generation = protocol::readUint32Le(data + 6);
    return true;
}

bool Transaction::load() {
    record_ = Record{};
    generation_ = 0;
    healthy_ = false;
    uint8_t bytes[kSnapshotSize]; size_t size = 0;
    const auto status = storage_.read(bytes, sizeof(bytes), size);
    if (status == replay::ReadStatus::Missing) { healthy_ = true; return true; }
    if (status != replay::ReadStatus::Ok || !decode(bytes, size, record_, generation_)) return false;
    healthy_ = true;
    return true;
}

bool Transaction::commit(const Record& record) {
    uint8_t bytes[kSnapshotSize], readback[kSnapshotSize]; size_t size = 0;
    const uint32_t next = generation_ + 1;
    if (!encode(record, next, bytes, sizeof(bytes))) return false;
    if (!storage_.write(bytes, sizeof(bytes)) ||
        storage_.read(readback, sizeof(readback), size) != replay::ReadStatus::Ok ||
        size != sizeof(bytes) || memcmp(bytes, readback, sizeof(bytes)) != 0) {
        healthy_ = false;
        return false;
    }
    record_ = record; generation_ = next; healthy_ = true;
    return true;
}

bool Transaction::clear() { return commit(Record{}); }

Status Transaction::request(const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
                            Transport transport, const uint8_t* wire, size_t size,
                            uint8_t nodeId, uint8_t networkId, Transport acceptTransport,
                            uint8_t* output, size_t capacity) {
    if (!healthy_) return Status::StorageError;
    if (!output || capacity < frames::kJoinAcceptSize) return Status::OutputTooSmall;
    frames::JoinRequest request;
    if (transport.sender != 0 || transport.target != 100 ||
        !frames::openJoinRequest(factoryMac, transport, wire, size, request)) return Status::InvalidFrame;
    if (!expectedUid || memcmp(request.identity.uid, expectedUid, protocol::kDeviceUidSize) != 0)
        return Status::WrongUid;
    if (record_.state != State::Empty) {
        if (memcmp(record_.request + 1, expectedUid, protocol::kDeviceUidSize) != 0) return Status::WrongUid;
        if (record_.state == State::Active) return Status::ActiveNode;
        if (memcmp(record_.request + 11, wire + 11, 8) == 0) {
            if (!same(record_.requestTransport, transport) || memcmp(record_.request, wire, size) != 0)
                return Status::ConflictingRequest;
            memcpy(output, record_.accept, sizeof(record_.accept));
            return Status::RepeatedAccept;
        }
        if (static_cast<uint32_t>(request.identity.requestNonce) <= protocol::readUint32Le(record_.request + 11))
            return Status::RetiredCounter;
    }
    if (nodeId < 1 || nodeId > 99 || !networkId || acceptTransport.target != 0 ||
        acceptTransport.sender != 100) return Status::InvalidAssignment;
    Record candidate;
    candidate.state = State::Pending;
    candidate.requestTransport = transport;
    candidate.acceptTransport = acceptTransport;
    memcpy(candidate.request, wire, size);
    frames::JoinAccept accept;
    accept.identity = request.identity; accept.nodeId = nodeId; accept.networkId = networkId;
    if (!random_.fill(accept.salt, kSaltSize)) return Status::StorageError;
    // Derive with the same factory CMAC; the factory key is never persisted.
    uint8_t input[1 + kSaltSize]; memcpy(input + 1, accept.salt, kSaltSize);
    input[0] = 1; osk::crypto::cmacCompute(factoryMac, input, sizeof(input), candidate.keys.encryption);
    input[0] = 2; osk::crypto::cmacCompute(factoryMac, input, sizeof(input), candidate.keys.authentication);
    if (!frames::sealJoinAccept(factoryMac, acceptTransport, accept, candidate.accept, sizeof(candidate.accept)))
        return Status::InvalidAssignment;
    if (!commit(candidate)) return Status::StorageError;
    memcpy(output, record_.accept, sizeof(record_.accept));
    return Status::Accept;
}

Status Transaction::confirm(Transport transport, const uint8_t* wire, size_t size,
                            BoundInitializer& bound, uint8_t* output, size_t capacity) {
    if (!healthy_) return Status::StorageError;
    if (!output || capacity < frames::kJoinCompleteSize) return Status::OutputTooSmall;
    if (record_.state == State::Empty || transport.target != 100 || transport.sender != record_.accept[27])
        return Status::InvalidFrame;
    osk::crypto::Cmac mac; osk::crypto::cmacInit(mac, record_.keys.authentication);
    uint8_t salt[kSaltSize]; memcpy(salt, record_.accept + 19, kSaltSize);
    frames::JoinIdentity proof;
    if (!frames::openJoinProof(mac, transport, wire, size, salt, false, proof) ||
        memcmp(proof.uid, record_.request + 1, protocol::kDeviceUidSize) != 0 ||
        static_cast<uint32_t>(proof.requestNonce) != protocol::readUint32Le(record_.request + 11) ||
        static_cast<uint32_t>(proof.requestNonce >> 32) != protocol::readUint32Le(record_.request + 15))
        return Status::InvalidFrame;
    const bool repeated = record_.state == State::Active;
    uint8_t complete[frames::kJoinCompleteSize];
    if (!frames::sealJoinProof(mac, Transport{record_.accept[27],100,0}, proof, salt, true,
                               complete, sizeof(complete))) return Status::InvalidFrame;
    if (!repeated) {
        if (!bound.ensure(record_.keys)) return Status::StorageError;
        Record candidate = record_; candidate.state = State::Active;
        if (!commit(candidate)) return Status::StorageError;
    }
    memcpy(output, complete, sizeof(complete));
    return repeated ? Status::RepeatedComplete : Status::Complete;
}

}  // namespace pairing
}  // namespace security
}  // namespace radiosensors
