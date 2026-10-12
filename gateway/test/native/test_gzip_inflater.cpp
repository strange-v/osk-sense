#include "GzipInflater.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using gateway::gzip::Inflater;
using Bytes = std::vector<uint8_t>;

constexpr size_t kWindow = 4096;

Bytes readFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool collect(void *context, const uint8_t *data, size_t size) {
    auto *output = static_cast<Bytes *>(context);
    output->insert(output->end(), data, data + size);
    return true;
}

bool refuse(void *, const uint8_t *, size_t) {
    return false;
}

struct Result {
    bool written;
    bool finished;
    Bytes output;
};

Result inflate(const Bytes &input, size_t chunk, size_t window = kWindow) {
    Result result{true, false, {}};
    Inflater inflater;
    assert(inflater.begin(window, collect, &result.output));
    for (size_t offset = 0; offset < input.size() && result.written; offset += chunk) {
        const size_t size = std::min(chunk, input.size() - offset);
        result.written = inflater.write(input.data() + offset, size);
    }
    result.finished = result.written && inflater.finish();
    assert(inflater.outputSize() == result.output.size());
    return result;
}

int main(int argc, char **argv) {
    // Written by run_native_tests_wsl.sh with Python's zlib.
    assert(argc > 1);
    const std::string directory = argv[1];
    const Bytes plain = readFile(directory + "/plain.bin");
    const Bytes small = readFile(directory + "/window-4k.gz");
    const Bytes large = readFile(directory + "/window-32k.gz");

    for (size_t chunk :
         {size_t(1), size_t(3), size_t(64), size_t(1000), size_t(4096), small.size()}) {
        const Result result = inflate(small, chunk);
        assert(result.written && result.finished && result.output == plain);
    }
    std::cout << "Gzip: 4 KB window stream inflated in chunks of 1 B to the whole file\n";

    // Matches farther back than the buffer: wrong bytes, no error. The caller's
    // hash is what catches this.
    const Result wrong = inflate(large, 4096);
    assert(!(wrong.written && wrong.finished && wrong.output == plain));
    const Result right = inflate(large, 4096, 32768);
    assert(right.written && right.finished && right.output == plain);
    std::cout << "Gzip: 32 KB window stream differs with a 4 KB buffer and inflates with 32 KB\n";

    Bytes changed = small;
    changed[3] = 0x08; // FNAME flag.
    assert(!inflate(changed, 4096).written);
    changed = small;
    changed[0] = 0x1e;
    assert(!inflate(changed, 4096).written);
    changed = small;
    changed[2] = 7;
    assert(!inflate(changed, 4096).written);

    for (size_t cut : {size_t(5), size_t(10), small.size() / 2, small.size() - 9}) {
        const Result result = inflate(Bytes(small.begin(), small.begin() + cut), 4096);
        assert(!result.finished);
    }
    Bytes extra = small;
    extra.push_back(0);
    assert(!inflate(extra, 4096).written);
    assert(!inflate(extra, 1).written);

    Inflater refusing;
    assert(refusing.begin(kWindow, refuse, nullptr));
    assert(!refusing.write(small.data(), small.size()));
    Inflater invalid;
    assert(!invalid.begin(3000, collect, nullptr));
    assert(!invalid.begin(0, collect, nullptr));
    std::cout << "Gzip: bad header, truncation, trailing data and sink failure rejected\n";
}
