#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gateway::password_hash {

constexpr uint32_t kDefaultIterations = 25000;

bool begin();

bool computePbkdf2Sha256(const char *password,
                         size_t passwordLength,
                         const uint8_t *salt,
                         size_t saltLength,
                         uint32_t iterations,
                         uint8_t *output,
                         size_t outputLength);

} // namespace gateway::password_hash
