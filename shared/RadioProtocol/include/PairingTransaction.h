#pragma once

#include "GatewayReplay.h"
#include "RadioSecurityFrames.h"

namespace radiosensors {
namespace security {
namespace pairing {

constexpr size_t kSnapshotSize = 123;
enum class State : uint8_t { Empty, Pending, Active };
struct Record {
    State state = State::Empty;
    Transport requestTransport{};
    Transport acceptTransport{};
    uint8_t request[frames::kJoinRequestSize]{};
    uint8_t accept[frames::kJoinAcceptSize]{};
    Keys keys{};
};
bool encode(const Record& record, uint32_t generation, uint8_t* output, size_t size);
bool decode(const uint8_t* data, size_t size, Record& record, uint32_t& generation);

// The registry adapter owns the UID slot and verifies that its bound belongs
// to these pending keys. An already prepared bound is success without reset.
class BoundInitializer {
public:
    virtual ~BoundInitializer() {}
    virtual bool ensure(const Keys& keys) = 0;
};

enum class Status : uint8_t {
    Accept, RepeatedAccept, Complete, RepeatedComplete,
    InvalidFrame, WrongUid, ConflictingRequest, RetiredCounter,
    ActiveNode, InvalidAssignment, OutputTooSmall, StorageError
};

class Transaction {
public:
    Transaction(replay::BlobStorage& storage, replay::RandomSource& random)
        : storage_(storage), random_(random) {}
    bool load();
    // Explicit registry deletion/re-enrolment; never a fallback for corrupt data.
    bool clear();
    const Record& record() const { return record_; }
    uint32_t generation() const { return generation_; }
    // Call only within the pairing window for expectedUid and its factory MAC.
    // New attempts use strictly increasing node frame counters in nonce's low word.
    Status request(const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
                   Transport transport, const uint8_t* wire, size_t size,
                   uint8_t nodeId, uint8_t networkId, Transport acceptTransport,
                   uint8_t* output, size_t capacity);
    Status confirm(Transport transport, const uint8_t* wire, size_t size,
                   BoundInitializer& bound, uint8_t* output, size_t capacity);
private:
    bool commit(const Record& record);
    replay::BlobStorage& storage_;
    replay::RandomSource& random_;
    Record record_{};
    uint32_t generation_ = 0;
    bool healthy_ = false;
};

}  // namespace pairing
}  // namespace security
}  // namespace radiosensors
