#pragma once
#include <stddef.h>
#include <string>
class Preferences {
public:
    bool begin(const char* space, bool) { space_ = space; return true; }
    bool isKey(const char* key);
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* output, size_t capacity);
    size_t putBytes(const char* key, const void* data, size_t size);
private:
    std::string space_;
};
