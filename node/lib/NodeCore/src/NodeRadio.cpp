#include "NodeRadio.h"

#include <Arduino.h>
#include <RFM69registers.h>
#include <string.h>

#include "DebugLog.h"

namespace radiosensors {
namespace node {

namespace {
constexpr uint32_t kAckWaitMs = 40;

}

NodeRadio::NodeRadio(const uint8_t chipSelect, const uint8_t interruptPin)
    : radio_(chipSelect, interruptPin) {}

bool NodeRadio::begin(const uint8_t nodeId, const uint8_t networkId) {
    const bool initialized =
        radio_.initialize(NODE_RFM69_FREQUENCY, nodeId, networkId);
    radio_.setHighPower(true);
    // The RFM69 releases MISO while deselected, which would leave the input
    // floating through every sleep. The weak pull-up does not disturb SPI.
    pinMode(PIN_SPI_MISO, INPUT_PULLUP);
    if (initialized) frequencyMsb_ = radio_.readReg(REG_FRFMSB);
    return initialized;
}

void NodeRadio::useProfile(const uint8_t address, const uint8_t network) {
    address_ = address;
    network_ = network;
    radio_.setAddress(address);
    radio_.setNetwork(network);
    radio_.encrypt(nullptr);
    setPowerLevel(NODE_RADIO_MAX_POWER_LEVEL);
}

void NodeRadio::setPowerLevel(const uint8_t level) {
    level_ = level;
    radio_.setPowerLevel(level);
}

// The module has no reset line, and restarting the MCU would not restore its
// registers either: rewriting them is the only repair. A module that reset
// itself comes back on its default frequency, with AES off and sync value 1.
bool NodeRadio::configured() {
    return radio_.readReg(REG_FRFMSB) == frequencyMsb_ &&
        (radio_.readReg(REG_PACKETCONFIG2) & RF_PACKET2_AES_ON) == 0 &&
        radio_.readReg(REG_SYNCVALUE2) == network_;
}

bool NodeRadio::ensureConfigured() {
    if (configured()) return true;
#if defined(NODE_DEBUG)
    debugLine(F("rf lost"));
#endif
    // initialize() also resynchronizes the library's cached mode with the
    // module, which a reset left in standby.
    if (!begin(address_, network_)) {
        radio_.sleep();
        return false;
    }
    radio_.encrypt(nullptr);
    radio_.setPowerLevel(level_);
    radio_.sleep();
    return configured();
}

bool NodeRadio::sendAcknowledged(
    const uint8_t gatewayId, const uint8_t* frame, const uint8_t size,
    const uint8_t attempts, const osk::crypto::Cmac& mac, const uint32_t counter, security::Ack& ack,
    int8_t& downlinkRssi) {
    ack = security::Ack{};
    // sendWithRetry() without its RSSI: the RFM69 keeps measuring the channel
    // after a frame ends, so a read after reception sees anything from the
    // frame to the noise floor. Sample it when the sync word matches, while
    // the acknowledgement is still arriving.
    for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
        radio_.send(gatewayId, frame, size, true);
        const uint32_t sent = millis();
        bool synced = false;
        int8_t rssi = protocol::kNoDownlinkRssi;
        while (static_cast<uint32_t>(millis() - sent) < kAckWaitMs) {
            const bool sync = (radio_.readReg(REG_IRQFLAGS1) &
                               RF_IRQFLAGS1_SYNCADDRESSMATCH) != 0;
            if (sync && !synced) {
                rssi = protocol::downlinkRssiValue(radio_.readRSSI());
            }
            synced = sync;
            if (!radio_.ACKReceived(gatewayId)) continue;
            // The acknowledgement stays in DATA until the radio receives
            // again.
            if (radio_.TARGETID != address_ || radio_.control != 0x80 ||
                !security::openAck(mac,receivedTransport(),counter,
                                   radio_.DATA,radio_.DATALEN,ack)) continue;
            downlinkRssi = rssi;
            radio_.sleep();
            return true;
        }
    }
    radio_.sleep();
    return false;
}

void NodeRadio::send(
    const uint8_t recipient, const uint8_t* frame, const uint8_t size) {
    radio_.send(recipient, frame, size, false);
}

bool NodeRadio::receive(
    const uint32_t timeoutMs, const uint8_t expectedSender, uint8_t* output,
    const uint8_t expectedSize) {
    return receiveMatching(
               timeoutMs, expectedSender, output, expectedSize,
               expectedSize) == expectedSize;
}

uint8_t NodeRadio::receiveFrame(
    const uint32_t timeoutMs, const uint8_t expectedSender, uint8_t* output,
    const uint8_t capacity) {
    return receiveMatching(timeoutMs, expectedSender, output, 1, capacity);
}

uint8_t NodeRadio::receiveMatching(
    const uint32_t timeoutMs, const uint8_t expectedSender, uint8_t* output,
    const uint8_t minimumSize, const uint8_t maximumSize) {
    const uint32_t started = millis();
    radio_.receiveDone();
    while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
        if (!radio_.receiveDone()) continue;
        const uint8_t size = radio_.DATALEN;
        if (radio_.TARGETID == address_ && radio_.SENDERID == expectedSender && size >= minimumSize &&
            size <= maximumSize) {
            memcpy(output, radio_.DATA, size);
            return size;
        }
        radio_.receiveDone();
    }
    return 0;
}

void NodeRadio::sleep() {
    radio_.sleep();
}

security::Transport NodeRadio::receivedTransport() const {
    return security::Transport{static_cast<uint8_t>(radio_.TARGETID),
                               static_cast<uint8_t>(radio_.SENDERID),radio_.control};
}

}  // namespace node
}  // namespace radiosensors
