#pragma once

#include <GatewayStorage.h>

#include "BackupCodec.h"

namespace gateway::backup {

// Excludes concurrent export and import. Holds nothing when another is running.
class Operation {
  public:
    Operation();
    ~Operation();
    Operation(const Operation &) = delete;
    Operation &operator=(const Operation &) = delete;

    explicit operator bool() const {
        return held_;
    }

  private:
    bool held_;
};

bool busy();
bool capture(Snapshot &snapshot);
bool cleanGateway();
bool restore(const Snapshot &snapshot,
             const radiosensors::gateway_storage::AuthenticationData &admin);

} // namespace gateway::backup
