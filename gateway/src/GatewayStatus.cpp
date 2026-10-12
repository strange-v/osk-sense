#include "GatewayStatus.h"

#include <ResetButton.h>

#include <atomic>

#include "BoardProfile.h"
#include "RadioService.h"
#include "ConfigurationStore.h"
#include "RecoveryService.h"

namespace gateway::status {
namespace {

constexpr uint32_t kDebounceMs = 50;
constexpr uint8_t kBrightness = 20;

#if defined(GATEWAY_BOARD_WAVESHARE_S3_ETH)
constexpr int kRgbPin = 21;
#endif

std::atomic<Indication> current{Indication::Operational};
std::atomic<uint32_t> pairingEndsAt{0};
std::atomic<uint32_t> setupEndsAt{0};
std::atomic<uint32_t> indicationEndsAt{0};
radiosensors::ResetButton resetButton;
bool resetArmed = false;
bool resetWaiting = false;
bool rawButtonPressed = false;
bool stableButtonPressed = false;
uint32_t rawButtonChangedAt = 0;

bool deadlineReached(const uint32_t now, const uint32_t deadline) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

Indication idleIndication() {
    return setupRequired() ? Indication::Unconfigured : Indication::Operational;
}

void setRgb(const uint8_t red, const uint8_t green, const uint8_t blue) {
#if defined(GATEWAY_BOARD_WAVESHARE_S3_ETH)
    rgbLedWrite(kRgbPin, red, green, blue);
#else
    (void)red;
    (void)green;
    (void)blue;
#endif
}

void togglePairing(const uint32_t now) {
    if (pairingActive()) {
        if (!radio::requestProfile(radio::Profile::Operational)) {
            current.store(Indication::Error);
            indicationEndsAt.store(now + 3000);
            Serial.println("Pairing close failed: radio profile did not switch");
            return;
        }
        pairingEndsAt.store(0);
        current.store(idleIndication());
        Serial.println("Pairing window closed by button");
    } else {
        Serial.println("Pairing requires QR credentials from the management UI");
    }
    indicationEndsAt.store(0);
}

void toggleSetup(const uint32_t now) {
    if (setupActive()) {
        setupEndsAt.store(0);
        current.store(Indication::Unconfigured);
        Serial.println("Initial setup window closed by button");
        return;
    }
    const radiosensors::gateway_storage::GatewaySettings settings = configuration_store::settings();
    setupEndsAt.store(now + static_cast<uint32_t>(settings.setupWindowSeconds) * 1000U);
    current.store(Indication::Setup);
    indicationEndsAt.store(0);
    Serial.printf("Initial setup window opened by button for %u seconds\n",
                  settings.setupWindowSeconds);
}

void render(const uint32_t now) {
    const uint32_t phase = now % 1000;
    if (resetButton.confirming() || recovery::blocked()) {
        setRgb(phase % 250 < 125 ? kBrightness : 0, 0, 0);
        return;
    }
    switch (current.load()) {
    case Indication::Unconfigured:
        setRgb(0, phase < 500 ? kBrightness : 0, 0);
        break;
    case Indication::Operational:
        setRgb(0, kBrightness, 0);
        break;
    case Indication::Setup: {
        const uint32_t triangle = phase < 500 ? phase : 1000 - phase;
        const uint8_t level = static_cast<uint8_t>(8 + triangle * 40 / 500);
        setRgb(level, 0, level);
        break;
    }
    case Indication::Pairing: {
        const uint32_t triangle = phase < 500 ? phase : 1000 - phase;
        const uint8_t level = static_cast<uint8_t>(8 + triangle * 64 / 500);
        setRgb(0, 0, level);
        break;
    }
    case Indication::PersistingNode:
        setRgb(phase < 300 ? kBrightness : 0, phase < 300 ? 24 : 0, 0);
        break;
    case Indication::AwaitingConfirm:
        setRgb(0, phase % 250 < 125 ? kBrightness : 0, phase % 250 < 125 ? kBrightness : 0);
        break;
    case Indication::PairingSucceeded:
        setRgb(kBrightness, kBrightness, kBrightness);
        break;
    case Indication::Error:
        setRgb(phase % 200 < 100 ? kBrightness : 0, 0, 0);
        break;
    case Indication::ProfileConflict: {
        const bool on = phase < 100 || (phase >= 200 && phase < 300);
        setRgb(on ? kBrightness : 0, 0, on ? kBrightness : 0);
        break;
    }
    }
}

} // namespace

void begin() {
    pinMode(board::buttonPin, INPUT_PULLUP);
    rawButtonPressed = digitalRead(board::buttonPin) == LOW;
    stableButtonPressed = rawButtonPressed;
    rawButtonChangedAt = millis();
    current.store(idleIndication());
    render(millis());
}

void loop() {
    const uint32_t now = millis();
    if (deadlineReached(now, pairingEndsAt.load())) {
        const bool switched = radio::requestProfile(radio::Profile::Operational);
        pairingEndsAt.store(0);
        if (switched) {
            if (indicationEndsAt.load() == 0)
                current.store(idleIndication());
            Serial.println("Pairing window expired");
        } else {
            current.store(Indication::Error);
            indicationEndsAt.store(now + 3000);
            Serial.println("Pairing expiry failed: radio profile did not switch");
        }
    }
    if (deadlineReached(now, setupEndsAt.load())) {
        setupEndsAt.store(0);
        current.store(idleIndication());
        Serial.println("Initial setup window expired");
    }
    if (deadlineReached(now, indicationEndsAt.load())) {
        indicationEndsAt.store(0);
        current.store(pairingActive() ? Indication::Pairing : idleIndication());
    }

    const bool pressed = digitalRead(board::buttonPin) == LOW;
    if (pressed != rawButtonPressed) {
        rawButtonPressed = pressed;
        rawButtonChangedAt = now;
    }
    if (pressed != stableButtonPressed && now - rawButtonChangedAt >= kDebounceMs)
        stableButtonPressed = pressed;
    const auto action = resetButton.update(stableButtonPressed, now);
    const bool confirmed = action == radiosensors::ResetButton::Action::ConfirmReset;
    if (resetButton.armed() && !resetArmed)
        Serial.println("Factory reset armed: release the button, then press it again within 5 s");
    if (resetButton.waiting() && !resetWaiting)
        Serial.println("Factory reset: waiting 5 s for the confirming press");
    if (resetWaiting && !resetButton.waiting() && !confirmed)
        Serial.println("Factory reset cancelled: no confirming press");
    resetArmed = resetButton.armed();
    resetWaiting = resetButton.waiting();
    if (confirmed) {
        Serial.println("Factory reset confirmed by button");
        if (!recovery::requestFactoryReset()) {
            current.store(Indication::Error);
            indicationEndsAt.store(now + 3000);
        }
    } else if (action == radiosensors::ResetButton::Action::ShortPress && !recovery::blocked()) {
        recovery::Guard guard(0);
        if (guard) {
            if (setupRequired())
                toggleSetup(now);
            else
                togglePairing(now);
        }
    }
    render(now);
}

bool pairingActive() {
    const uint32_t deadline = pairingEndsAt.load();
    return deadline != 0 && static_cast<int32_t>(deadline - millis()) > 0;
}

uint32_t pairingRemainingSeconds() {
    if (!pairingActive())
        return 0;
    return (pairingEndsAt.load() - millis() + 999) / 1000;
}

bool setupRequired() {
    return recovery::blocked() || configuration_store::authentication().userCount == 0;
}

bool setupActive() {
    if (recovery::blocked() || !setupRequired())
        return false;
    const uint32_t deadline = setupEndsAt.load();
    return deadline != 0 && static_cast<int32_t>(deadline - millis()) > 0;
}

uint32_t setupRemainingSeconds() {
    if (!setupActive())
        return 0;
    return (setupEndsAt.load() - millis() + 999) / 1000;
}

void closeSetup() {
    setupEndsAt.store(0);
    current.store(Indication::Operational);
    indicationEndsAt.store(0);
}

bool closePairing() {
    pairingEndsAt.store(0);
    if (radio::requestProfile(radio::Profile::Operational)) {
        current.store(Indication::Operational);
        indicationEndsAt.store(0);
        return true;
    } else {
        current.store(Indication::Error);
        indicationEndsAt.store(millis() + 3000);
        Serial.println("Pairing completion failed: radio profile did not switch");
        return false;
    }
}

bool openPairing() {
    const auto settings = configuration_store::settings();
    pairingEndsAt.store(millis() + static_cast<uint32_t>(settings.pairingWindowSeconds) * 1000U);
    current.store(Indication::Pairing);
    indicationEndsAt.store(0);
    Serial.printf("Pairing window opened for %u seconds\n", settings.pairingWindowSeconds);
    return true;
}

void indicate(const Indication indication, const uint32_t durationMs) {
    current.store(indication);
    indicationEndsAt.store(durationMs == 0 ? 0 : millis() + durationMs);
}

const char *indicationName() {
    switch (current.load()) {
    case Indication::Unconfigured:
        return "unconfigured";
    case Indication::Operational:
        return "operational";
    case Indication::Setup:
        return "setup";
    case Indication::Pairing:
        return "pairing";
    case Indication::PersistingNode:
        return "persisting_node";
    case Indication::AwaitingConfirm:
        return "awaiting_confirm";
    case Indication::PairingSucceeded:
        return "pairing_succeeded";
    case Indication::Error:
        return "error";
    case Indication::ProfileConflict:
        return "profile_conflict";
    }
    return "unknown";
}

} // namespace gateway::status
