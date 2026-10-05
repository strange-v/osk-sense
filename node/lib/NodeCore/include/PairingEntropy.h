#pragma once

#include <RadioCrypto.h>
#include <JoinRequest.h>
#include <string.h>

namespace radiosensors {
namespace node {
namespace pairing_entropy {

constexpr uint16_t kSampleCount = 512;
constexpr uint16_t kWindow = 128;
constexpr uint8_t kMaterialSize = 8;

// Health checks detect stuck or strongly biased input; they do not establish
// a minimum entropy bound or validate independence between samples.
class Extractor {
public:
    bool push(uint16_t capture) {
        if (failed_ || samples_ == kSampleCount) return false;
        const uint8_t bit = capture & 1;
        run_ = samples_ && bit == previous_ ? static_cast<uint8_t>(run_ + 1) : 1;
        previous_ = bit;
        if (run_ > 32) { failed_ = true; return false; }
        windowOnes_ += bit;
        ++samples_;
        if (samples_ % kWindow == 0) {
            if (windowOnes_ < 32 || windowOnes_ > 96) { failed_ = true; return false; }
            windowOnes_ = 0;
        }
        if (samples_ & 1) pairFirst_ = bit;
        else if (pairFirst_ != bit && extracted_ < kMaterialSize * 8) {
            extractedRun_ = extracted_ && pairFirst_ == extractedPrevious_ ?
                static_cast<uint8_t>(extractedRun_ + 1) : 1;
            extractedPrevious_ = pairFirst_;
            if (extractedRun_ > 16) { failed_ = true; return false; }
            material_[extracted_ / 8] |= pairFirst_ << (extracted_ % 8);
            extractedOnes_ += pairFirst_; ++extracted_;
        }
        return true;
    }
    bool finish(uint8_t (&out)[kMaterialSize]) const {
        if (failed_ || samples_ != kSampleCount || extracted_ != kMaterialSize * 8 ||
            extractedOnes_ < 16 || extractedOnes_ > 48) return false;
        memcpy(out,material_,sizeof(material_)); return true;
    }
private:
    uint8_t material_[kMaterialSize]{};
    uint16_t samples_ = 0;
    uint8_t previous_ = 0, run_ = 0, windowOnes_ = 0, pairFirst_ = 0;
    uint8_t extracted_ = 0, extractedPrevious_ = 0, extractedRun_ = 0, extractedOnes_ = 0;
    bool failed_ = false;
};

// SampleSource::next(uint16_t&) must return false on a bounded capture timeout.
// Never substitutes micros(), stale samples or deterministic data on failure.
template <typename SampleSource>
bool collect(SampleSource& source, uint8_t (&out)[kMaterialSize]) {
    Extractor extractor;
    for (uint16_t i = 0; i < kSampleCount; ++i) {
        uint16_t capture;
        if (!source.next(capture) || !extractor.push(capture)) return false;
    }
    return extractor.finish(out);
}

inline uint32_t condition(const osk::crypto::Cmac& factoryMac,
                          const uint8_t (&uid)[protocol::kDeviceUidSize], uint32_t counter,
                          const uint8_t (&material)[kMaterialSize]) {
    uint8_t input[1 + protocol::kDeviceUidSize + 4 + kMaterialSize], tag[16];
    input[0] = 3;
    memcpy(input + 1,uid,sizeof(uid));
    protocol::writeUint32Le(input + 1 + sizeof(uid),counter);
    memcpy(input + 1 + sizeof(uid) + 4,material,sizeof(material));
    osk::crypto::cmacCompute(factoryMac,input,sizeof(input),tag);
    return protocol::readUint32Le(tag);
}

}  // namespace pairing_entropy
}  // namespace node
}  // namespace radiosensors
