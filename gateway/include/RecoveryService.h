#pragma once

#include <Arduino.h>

namespace gateway::recovery {

// All persistent mutations take this lock before their store-specific lock.
class Guard {
  public:
    explicit Guard(TickType_t wait = portMAX_DELAY);
    ~Guard();
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

    explicit operator bool() const {
        return held_;
    }

  private:
    bool held_;
};

void begin();
bool blocked();
const char *reason();
bool startRestore();
bool eraseInstallation();
bool finishRestore();
bool requestFactoryReset();
void restartSoon();
void loop();

} // namespace gateway::recovery
