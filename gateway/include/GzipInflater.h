#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gateway::gzip {

// Inflates a gzip stream fed in chunks of any size and passes the output to a
// sink. The circular output buffer is the deflate window: a stream compressed
// with a larger window inflates to wrong bytes without an error, so the caller
// checks the output against a hash.
class Inflater {
  public:
    using Sink = bool (*)(void *context, const uint8_t *data, size_t size);

    Inflater() = default;
    ~Inflater();
    Inflater(const Inflater &) = delete;
    Inflater &operator=(const Inflater &) = delete;

    // `windowSize` is a power of two. False when memory is short.
    bool begin(size_t windowSize, Sink sink, void *context);
    // False on a malformed stream, a sink failure, or data after the stream.
    bool write(const uint8_t *data, size_t size);
    // True once the deflate stream has ended.
    bool finish() const;
    size_t outputSize() const {
        return output_;
    }

  private:
    enum class State : uint8_t { Header, Deflate, Trailer, Failed };

    bool fail();

    State state_ = State::Header;
    void *decompressor_ = nullptr;
    uint8_t *window_ = nullptr;
    size_t windowSize_ = 0;
    size_t position_ = 0;
    size_t output_ = 0;
    uint8_t header_[10]{};
    size_t headerSize_ = 0;
    size_t trailerSize_ = 0;
    Sink sink_ = nullptr;
    void *context_ = nullptr;
};

} // namespace gateway::gzip
