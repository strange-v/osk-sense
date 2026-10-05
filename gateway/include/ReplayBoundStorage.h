#pragma once

#include <GatewayReplay.h>
#include <Preferences.h>

namespace gateway {

// Owned by the radio task. Backup codecs must never export this blob.
class ReplayBoundStorage final : public radiosensors::replay::BlobStorage {
public:
    bool begin();
    radiosensors::replay::ReadStatus read(
        uint8_t* output, size_t capacity, size_t& size) override;
    bool write(const uint8_t* data, size_t size) override;
private:
    Preferences preferences_;
    bool initialized_ = false;
};

}  // namespace gateway
