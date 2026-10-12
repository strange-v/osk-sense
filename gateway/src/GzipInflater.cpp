#include "GzipInflater.h"

// On the gateway this is the inflater in the ESP32 ROM.
#include <miniz.h>

#include <new>

namespace gateway::gzip {
namespace {

constexpr size_t kHeaderSize = 10;
constexpr size_t kTrailerSize = 8;

tinfl_decompressor *decompressor(void *pointer) {
    return static_cast<tinfl_decompressor *>(pointer);
}

} // namespace

Inflater::~Inflater() {
    delete decompressor(decompressor_);
    delete[] window_;
}

bool Inflater::begin(const size_t windowSize, Sink sink, void *context) {
    if (windowSize == 0 || (windowSize & (windowSize - 1)) != 0 || sink == nullptr)
        return false;
    decompressor_ = new (std::nothrow) tinfl_decompressor;
    window_ = new (std::nothrow) uint8_t[windowSize];
    if (decompressor_ == nullptr || window_ == nullptr)
        return fail();
    tinfl_init(decompressor(decompressor_));
    windowSize_ = windowSize;
    sink_ = sink;
    context_ = context;
    return true;
}

bool Inflater::fail() {
    state_ = State::Failed;
    return false;
}

bool Inflater::write(const uint8_t *data, size_t size) {
    while (size > 0) {
        switch (state_) {
        case State::Failed:
            return false;
        case State::Header: {
            header_[headerSize_++] = *data++;
            --size;
            // Only what make_manifest.py writes: deflate, no optional fields.
            if (headerSize_ == kHeaderSize) {
                if (header_[0] != 0x1f || header_[1] != 0x8b || header_[2] != 8 || header_[3] != 0)
                    return fail();
                state_ = State::Deflate;
            }
            break;
        }
        case State::Deflate: {
            size_t consumed = size;
            size_t produced = windowSize_ - position_;
            const tinfl_status status = tinfl_decompress(decompressor(decompressor_),
                                                         data,
                                                         &consumed,
                                                         window_,
                                                         window_ + position_,
                                                         &produced,
                                                         TINFL_FLAG_HAS_MORE_INPUT);
            data += consumed;
            size -= consumed;
            if (produced > 0) {
                if (!sink_(context_, window_ + position_, produced))
                    return fail();
                position_ = (position_ + produced) & (windowSize_ - 1);
                output_ += produced;
            }
            if (status == TINFL_STATUS_DONE)
                state_ = State::Trailer;
            else if (status < 0 || (consumed == 0 && produced == 0))
                return fail();
            break;
        }
        case State::Trailer:
            // The inflater may already hold trailer bytes in its bit
            // buffer, so the CRC and size fields are not read; the caller's
            // hash of the output is the integrity check.
            if (size > kTrailerSize - trailerSize_)
                return fail();
            trailerSize_ += size;
            size = 0;
            break;
        }
    }
    return state_ != State::Failed;
}

bool Inflater::finish() const {
    return state_ == State::Trailer;
}

} // namespace gateway::gzip
