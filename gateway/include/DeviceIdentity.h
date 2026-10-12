#pragma once

#include <stddef.h>

namespace gateway::identity {

constexpr size_t kIdentityCharacters = 32;

void begin();
const char *hostname();
const char *gatewayId();
const char *bootId();

} // namespace gateway::identity
