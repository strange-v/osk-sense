#pragma once

#if __has_include("LocalSecrets.h")
#include "LocalSecrets.h"
#endif

#ifndef GATEWAY_OTA_PASSWORD
#define GATEWAY_OTA_PASSWORD ""
#endif

namespace gateway::ota::config {

constexpr char password[] = GATEWAY_OTA_PASSWORD;
constexpr bool enabled = sizeof(password) > 1;

} // namespace gateway::ota::config
