#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace radiosensors {
namespace node {
namespace storage {

constexpr size_t kEepromSize = 256;
constexpr size_t kFactoryCredentialSize = 32;
constexpr size_t kFactoryKeySize = 16;
constexpr size_t kNetworkConfigSlotSize = 32;
constexpr size_t kNetworkConfigSlotA = 0x00;
constexpr size_t kNetworkConfigSlotB = 0x20;
constexpr size_t kProfileAreaStart = 0x40;
static_assert(kNetworkConfigSlotB + kNetworkConfigSlotSize == kProfileAreaStart,
              "profile EEPROM area must follow the network configuration slots");

enum class ProvisioningState : uint8_t {
    Provisional = 1,
    Active = 2,
};

struct NetworkConfig {
    uint8_t generation = 0;
    ProvisioningState state = ProvisioningState::Provisional;
    uint8_t nodeId = 0;
    uint8_t gatewayId = 0;
    uint8_t networkId = 0;
    uint8_t installationKey[16]{};
    uint32_t requestNonce = 0;
};

struct FactoryCredentials {
    uint8_t key[kFactoryKeySize]{};
};

inline uint16_t crc16Ccitt(const uint8_t *data, size_t size) {
    uint16_t crc = 0xFFFFU;
    while (size-- != 0) {
        crc ^= static_cast<uint16_t>(*data++) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                                       : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline void write16(uint8_t *output, const uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
}

inline void write32(uint8_t *output, const uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
    output[2] = static_cast<uint8_t>(value >> 16);
    output[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t read16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) | (static_cast<uint16_t>(input[1]) << 8);
}

inline uint32_t read32(const uint8_t *input) {
    return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8) |
           (static_cast<uint32_t>(input[2]) << 16) | (static_cast<uint32_t>(input[3]) << 24);
}

inline bool
encodeFactoryCredentials(const FactoryCredentials &value, uint8_t *output, const size_t capacity) {
    if (output == nullptr || capacity < kFactoryCredentialSize)
        return false;
    uint8_t combined = 0;
    for (size_t index = 0; index < sizeof(value.key); ++index)
        combined |= value.key[index];
    if (combined == 0)
        return false;
    memset(output, 0, kFactoryCredentialSize);
    output[0] = 'R';
    output[1] = 'S';
    output[2] = 'F';
    output[3] = 'C';
    output[4] = 1;
    output[5] = 1;
    memcpy(output + 8, value.key, sizeof(value.key));
    write16(output + 24, crc16Ccitt(output, 24));
    output[26] = static_cast<uint8_t>(~output[24]);
    output[27] = static_cast<uint8_t>(~output[25]);
    return true;
}

inline bool
decodeFactoryCredentials(const uint8_t *input, const size_t size, FactoryCredentials &value) {
    if (input == nullptr || size != kFactoryCredentialSize || input[0] != 'R' || input[1] != 'S' ||
        input[2] != 'F' || input[3] != 'C' || input[4] != 1 || input[5] != 1 || input[6] != 0 ||
        input[7] != 0 || input[26] != static_cast<uint8_t>(~input[24]) ||
        input[27] != static_cast<uint8_t>(~input[25]) ||
        read16(input + 24) != crc16Ccitt(input, 24))
        return false;
    for (size_t index = 28; index < kFactoryCredentialSize; ++index)
        if (input[index] != 0)
            return false;
    uint8_t combined = 0;
    for (size_t index = 0; index < kFactoryKeySize; ++index)
        combined |= input[8 + index];
    if (combined == 0)
        return false;
    memcpy(value.key, input + 8, sizeof(value.key));
    return true;
}

template <typename Storage> class FactoryCredentialStore {
  public:
    explicit FactoryCredentialStore(Storage &storage) : storage_(storage) {
    }

    bool load(FactoryCredentials &value) const {
        uint8_t bytes[kFactoryCredentialSize];
        for (size_t index = 0; index < sizeof(bytes); ++index)
            bytes[index] = storage_.read(index);
        return decodeFactoryCredentials(bytes, sizeof(bytes), value);
    }

  private:
    Storage &storage_;
};

inline bool
encodeNetworkConfig(const NetworkConfig &value, uint8_t *output, const size_t capacity) {
    constexpr uint8_t kMagic0 = 'R';
    constexpr uint8_t kMagic1 = 'N';
    constexpr uint8_t kSchema = 2;
    if (output == nullptr || capacity < kNetworkConfigSlotSize || value.nodeId == 0 ||
        value.nodeId == 255 || value.gatewayId == 0 || value.gatewayId == 255 ||
        value.gatewayId == value.nodeId) {
        return false;
    }
    const uint8_t state = static_cast<uint8_t>(value.state);
    if (state != static_cast<uint8_t>(ProvisioningState::Provisional) &&
        state != static_cast<uint8_t>(ProvisioningState::Active)) {
        return false;
    }

    memset(output, 0, kNetworkConfigSlotSize);
    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kSchema;
    output[3] = value.generation;
    output[4] = state;
    output[5] = value.nodeId;
    output[6] = value.gatewayId;
    output[7] = value.networkId;
    memcpy(output + 8, value.installationKey, sizeof(value.installationKey));
    write32(output + 24, value.requestNonce);
    write16(output + 30, crc16Ccitt(output, 30));
    return true;
}

inline bool decodeNetworkConfig(const uint8_t *input, const size_t size, NetworkConfig &value) {
    if (input == nullptr || size != kNetworkConfigSlotSize || input[0] != 'R' || input[1] != 'N' ||
        input[2] != 2 || read16(input + 30) != crc16Ccitt(input, 30)) {
        return false;
    }
    const uint8_t state = input[4];
    if ((state != static_cast<uint8_t>(ProvisioningState::Provisional) &&
         state != static_cast<uint8_t>(ProvisioningState::Active)) ||
        input[5] == 0 || input[5] == 255 || input[6] == 0 || input[6] == 255 ||
        input[5] == input[6] || input[28] != 0 || input[29] != 0) {
        return false;
    }
    value.generation = input[3];
    value.state = static_cast<ProvisioningState>(state);
    value.nodeId = input[5];
    value.gatewayId = input[6];
    value.networkId = input[7];
    memcpy(value.installationKey, input + 8, sizeof(value.installationKey));
    value.requestNonce = read32(input + 24);
    return true;
}

inline bool generationIsNewer(const uint8_t candidate, const uint8_t current) {
    return static_cast<int8_t>(candidate - current) > 0;
}

template <typename Storage>
inline void readBlock(Storage &storage, const size_t address, uint8_t *output, const size_t size) {
    for (size_t index = 0; index < size; ++index)
        output[index] = storage.read(address + index);
}

template <typename Storage> class NetworkConfigStore {
  public:
    explicit NetworkConfigStore(Storage &storage) : storage_(storage) {
    }

    bool load(NetworkConfig &value) {
        uint8_t first[kNetworkConfigSlotSize];
        uint8_t second[kNetworkConfigSlotSize];
        NetworkConfig a{};
        NetworkConfig b{};
        readBlock(storage_, kNetworkConfigSlotA, first, sizeof(first));
        readBlock(storage_, kNetworkConfigSlotB, second, sizeof(second));
        const bool validA = decodeNetworkConfig(first, sizeof(first), a);
        const bool validB = decodeNetworkConfig(second, sizeof(second), b);
        initialized_ = true;
        hasValue_ = validA || validB;
        if (!hasValue_) {
            currentSlot_ = 1;
            currentGeneration_ = 0;
            return false;
        }
        if (validA && (!validB || generationIsNewer(a.generation, b.generation))) {
            value = a;
            currentSlot_ = 0;
        } else {
            value = b;
            currentSlot_ = 1;
        }
        currentGeneration_ = value.generation;
        return true;
    }

    bool save(NetworkConfig &value) {
        if (!initialized_) {
            NetworkConfig ignored{};
            load(ignored);
        }
        value.generation = hasValue_ ? static_cast<uint8_t>(currentGeneration_ + 1U) : 0;
        uint8_t bytes[kNetworkConfigSlotSize];
        if (!encodeNetworkConfig(value, bytes, sizeof(bytes)))
            return false;
        const uint8_t nextSlot = static_cast<uint8_t>(currentSlot_ ^ 1U);
        const size_t address = nextSlot == 0 ? kNetworkConfigSlotA : kNetworkConfigSlotB;
        storage_.update(address, 0);
        storage_.update(address + 1, 0);
        for (size_t index = 2; index < sizeof(bytes); ++index)
            storage_.update(address + index, bytes[index]);
        storage_.update(address + 1, bytes[1]);
        storage_.update(address, bytes[0]);
        currentSlot_ = nextSlot;
        currentGeneration_ = value.generation;
        hasValue_ = true;
        return true;
    }

    void factoryReset() {
        storage_.update(kNetworkConfigSlotA, 0);
        storage_.update(kNetworkConfigSlotA + 1, 0);
        storage_.update(kNetworkConfigSlotB, 0);
        storage_.update(kNetworkConfigSlotB + 1, 0);
        initialized_ = true;
        hasValue_ = false;
        currentSlot_ = 1;
        currentGeneration_ = 0;
    }

  private:
    Storage &storage_;
    bool initialized_ = false;
    bool hasValue_ = false;
    uint8_t currentSlot_ = 1;
    uint8_t currentGeneration_ = 0;
};

} // namespace storage
} // namespace node
} // namespace radiosensors
