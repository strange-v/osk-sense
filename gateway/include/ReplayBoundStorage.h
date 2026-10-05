#pragma once

#include <GatewayReplay.h>
#include <Preferences.h>
#include <atomic>

namespace gateway {

// Owned by the radio task. Backup codecs must never export this blob.
class ReplayBoundStorage final : public radiosensors::replay::BlobStorage {
public:
    struct Diagnostics { uint32_t writes, failures, lastWriteUs, maxWriteUs; };
    bool begin();
    Diagnostics diagnostics() const;
    radiosensors::replay::ReadStatus read(
        uint8_t* output, size_t capacity, size_t& size) override;
    bool write(const uint8_t* data, size_t size) override;
private:
    Preferences preferences_;
    bool initialized_ = false;
    std::atomic<uint32_t> writes_{0}, failures_{0}, lastWriteUs_{0}, maxWriteUs_{0};
};

}  // namespace gateway
