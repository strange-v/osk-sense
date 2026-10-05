#include "NodeRegistry.h"

#include <string.h>

#include "RadioPowerControl.h"

namespace radiosensors {
namespace registry {

bool validDisplayName(const char* const value, const size_t length) {
    if ((value == nullptr && length != 0) || length > kNodeDisplayNameSize)
        return false;
    size_t index = 0;
    while (index < length) {
        const uint8_t first = static_cast<uint8_t>(value[index]);
        uint32_t codePoint = 0;
        size_t continuationCount = 0;
        if (first < 0x80) {
            codePoint = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            codePoint = first & 0x1F;
            continuationCount = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            codePoint = first & 0x0F;
            continuationCount = 2;
        } else if (first >= 0xF0 && first <= 0xF4) {
            codePoint = first & 0x07;
            continuationCount = 3;
        } else {
            return false;
        }
        if (index + continuationCount >= length) return false;
        for (size_t offset = 1; offset <= continuationCount; ++offset) {
            const uint8_t next = static_cast<uint8_t>(value[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF) ||
            codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

NodeRegistry::NodeRegistry() : records_{}, count_(0) {}

size_t NodeRegistry::size() const {
    return count_;
}

const NodeRecord* NodeRegistry::records() const {
    return records_;
}

bool NodeRegistry::uidEquals(const uint8_t* left, const uint8_t* right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    for (size_t index = 0; index < protocol::kDeviceUidSize; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool NodeRegistry::firmwareEquals(
    const protocol::FirmwareVersion& left,
    const protocol::FirmwareVersion& right) {
    return left.major == right.major &&
           left.minor == right.minor &&
           left.patch == right.patch;
}

const NodeRecord* NodeRegistry::findByUid(const uint8_t* deviceUid) const {
    for (size_t index = 0; index < count_; ++index) {
        if (uidEquals(records_[index].deviceUid, deviceUid)) {
            return &records_[index];
        }
    }
    return nullptr;
}

const NodeRecord* NodeRegistry::findByNodeId(const uint8_t nodeId) const {
    for (size_t index = 0; index < count_; ++index) {
        if (records_[index].nodeId == nodeId) {
            return &records_[index];
        }
    }
    return nullptr;
}

NodeRecord* NodeRegistry::findMutableByUid(const uint8_t* deviceUid) {
    return const_cast<NodeRecord*>(
        static_cast<const NodeRegistry*>(this)->findByUid(deviceUid));
}

NodeRecord* NodeRegistry::findMutableByNodeId(const uint8_t nodeId) {
    return const_cast<NodeRecord*>(
        static_cast<const NodeRegistry*>(this)->findByNodeId(nodeId));
}

bool NodeRegistry::disable(const uint8_t nodeId) {
    NodeRecord* record = findMutableByNodeId(nodeId);
    if (record == nullptr || record->state == NodeState::Disabled) {
        return false;
    }
    record->state = NodeState::Disabled;
    if (record->replaySlot == 255) record->requestNonce = 0;
    return true;
}

bool NodeRegistry::remove(const uint8_t nodeId) {
    for (size_t index = 0; index < count_; ++index) {
        if (records_[index].nodeId != nodeId) {
            continue;
        }
        records_[index] = records_[count_ - 1];
        records_[count_ - 1] = NodeRecord{};
        --count_;
        return true;
    }
    return false;
}

RenameStatus NodeRegistry::rename(
    const uint8_t nodeId, const char* const displayName, const size_t length) {
    if (!validDisplayName(displayName, length)) return RenameStatus::InvalidName;
    NodeRecord* const record = findMutableByNodeId(nodeId);
    if (record == nullptr) return RenameStatus::NotFound;
    if (record->displayNameLength == length &&
        (length == 0 || memcmp(record->displayName, displayName, length) == 0)) {
        return RenameStatus::NoChange;
    }
    memset(record->displayName, 0, sizeof(record->displayName));
    if (length != 0) memcpy(record->displayName, displayName, length);
    record->displayNameLength = static_cast<uint8_t>(length);
    return RenameStatus::Renamed;
}

PowerPolicyStatus NodeRegistry::setPowerPolicy(
    const uint8_t nodeId, const uint8_t policy) {
    NodeRecord* const record = findMutableByNodeId(nodeId);
    if (record == nullptr) return PowerPolicyStatus::NotFound;
    if (!radio_power::validPolicy(policy) ||
        (radio_power::isFixedPolicy(policy) &&
         radio_power::fixedLevel(policy) > record->maxPowerLevel)) {
        return PowerPolicyStatus::InvalidPolicy;
    }
    if (record->powerPolicy == policy) return PowerPolicyStatus::NoChange;
    record->powerPolicy = policy;
    return PowerPolicyStatus::Updated;
}

InfoStatus NodeRegistry::updateInfo(
    const uint8_t nodeId, const uint8_t* const deviceUid,
    const protocol::NodeInfo& info) {
    NodeRecord* const record = findMutableByNodeId(nodeId);
    if (record == nullptr || record->state != NodeState::Active ||
        !uidEquals(record->deviceUid, deviceUid)) {
        return InfoStatus::NotFound;
    }
    const bool profileChanged = record->profileId != info.profileId;
    if (!profileChanged && firmwareEquals(record->firmware, info.firmware) &&
        record->maxPowerLevel == info.maxPowerLevel) {
        return InfoStatus::NoChange;
    }
    record->profileId = info.profileId;
    record->firmware = info.firmware;
    record->maxPowerLevel = info.maxPowerLevel;
    if (radio_power::isFixedPolicy(record->powerPolicy) &&
        radio_power::fixedLevel(record->powerPolicy) > info.maxPowerLevel) {
        record->powerPolicy = radio_power::kPolicyAuto;
    }
    return profileChanged ? InfoStatus::ProfileChanged : InfoStatus::Updated;
}

bool NodeRegistry::restore(const NodeRecord* records, const size_t count) {
    if ((records == nullptr && count != 0) || count > kMaxNodes) {
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        const NodeRecord& record = records[index];
        if (record.replaySlot != 255) {
            security::pairing::Record pairing;
            uint32_t generation = 0;
            if (record.replaySlot >= replay::kNodeSlots ||
                !security::pairing::decode(record.pairing, sizeof(record.pairing), pairing, generation) ||
                pairing.state == security::pairing::State::Empty ||
                !uidEquals(record.deviceUid, pairing.request + 1) ||
                record.nodeId != pairing.accept[27] ||
                record.requestNonce != protocol::readUint32Le(pairing.request + 11) ||
                (record.state == NodeState::Pending && pairing.state != security::pairing::State::Pending) ||
                (record.state == NodeState::Active && pairing.state != security::pairing::State::Active)) return false;
        } else {
            for (const uint8_t byte : record.pairing) if (byte != 0) return false;
        }
        if (record.nodeId < kFirstNodeId || record.nodeId > kLastNodeId ||
            record.profileId == protocol::kUnassignedProfileId ||
            !validDisplayName(record.displayName, record.displayNameLength) ||
            record.maxPowerLevel > protocol::kMaxRadioPowerLevel ||
            !radio_power::validPolicy(record.powerPolicy) ||
            (record.state != NodeState::Pending &&
             record.state != NodeState::Active &&
             record.state != NodeState::Disabled)) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (records[previous].nodeId == record.nodeId ||
                (record.replaySlot != 255 && records[previous].replaySlot == record.replaySlot) ||
                uidEquals(records[previous].deviceUid, record.deviceUid)) {
                return false;
            }
        }
    }

    count_ = count;
    for (size_t index = 0; index < count; ++index) {
        records_[index] = records[index];
    }
    for (size_t index = count; index < kMaxNodes; ++index) {
        records_[index] = NodeRecord{};
    }
    return true;
}

bool NodeRegistry::setPairing(const security::pairing::Record& pairing,
                              const uint32_t generation, const uint8_t replaySlot) {
    using security::pairing::State;
    if (pairing.state == State::Empty || replaySlot >= replay::kNodeSlots) return false;
    uint8_t encoded[security::pairing::kSnapshotSize];
    if (!security::pairing::encode(pairing, generation, encoded, sizeof(encoded))) return false;
    NodeRecord* record = findMutableByUid(pairing.request + 1);
    if (!record) {
        if (count_ >= kMaxNodes || findByNodeId(pairing.accept[27])) return false;
        record = &records_[count_];
    } else if (record->nodeId != pairing.accept[27] || record->state == NodeState::Disabled) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i)
        if (&records_[i] != record && records_[i].replaySlot == replaySlot) return false;
    if (record == &records_[count_]) ++count_;
    memcpy(record->deviceUid, pairing.request + 1, sizeof(record->deviceUid));
    record->nodeId = pairing.accept[27];
    // ReadInfo may update an active node's profile independently of pairing.
    if (record->state != NodeState::Active) {
        record->profileId = protocol::readUint16Le(pairing.request + 19);
        record->firmware = {pairing.request[21], pairing.request[22], pairing.request[23]};
        record->maxPowerLevel = pairing.request[24];
        if (radio_power::isFixedPolicy(record->powerPolicy) &&
            radio_power::fixedLevel(record->powerPolicy) > record->maxPowerLevel)
            record->powerPolicy = radio_power::kPolicyAuto;
    }
    record->state = pairing.state == State::Active ? NodeState::Active : NodeState::Pending;
    record->requestNonce = protocol::readUint32Le(pairing.request + 11);
    record->replaySlot = replaySlot;
    memcpy(record->pairing, encoded, sizeof(encoded));
    return true;
}

}  // namespace registry
}  // namespace radiosensors
