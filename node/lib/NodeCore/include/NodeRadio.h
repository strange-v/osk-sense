#pragma once

#include <RFM69.h>
#include <TelemetryFrames.h>
#include <RadioSecurity.h>
#include <stddef.h>
#include <stdint.h>


namespace radiosensors {
namespace node {

class NodeRadio {
public:
    NodeRadio(uint8_t chipSelect, uint8_t interruptPin);

    bool begin(uint8_t nodeId, uint8_t networkId);
    void useProfile(uint8_t nodeId, uint8_t networkId);
    void setPowerLevel(uint8_t level);
    // Rewrites the applied profile when the module has lost its registers,
    // for instance after its own brown-out. Returns whether the module holds
    // that profile afterwards; call it before each exchange.
    bool ensureConfigured();
    // Up to `attempts` transmissions. `ack` is what the acknowledgement
    // carried, `downlinkRssi` its strength or protocol::kNoDownlinkRssi.
    bool sendAcknowledged(
        uint8_t gatewayId, const uint8_t* frame, uint8_t size, uint8_t attempts,
        const osk::crypto::Cmac& mac, uint32_t counter,
        security::Ack& ack, int8_t& downlinkRssi);
    void send(uint8_t recipient, const uint8_t* frame, uint8_t size);
    bool receive(
        uint32_t timeoutMs, uint8_t expectedSender, uint8_t* output,
        uint8_t expectedSize);
    // The first frame from the sender that fits; zero after the timeout.
    uint8_t receiveFrame(
        uint32_t timeoutMs, uint8_t expectedSender, uint8_t* output,
        uint8_t capacity);
    security::Transport receivedTransport() const;
    void sleep();

private:
    uint8_t receiveMatching(
        uint32_t timeoutMs, uint8_t expectedSender, uint8_t* output,
        uint8_t minimumSize, uint8_t maximumSize);
    bool configured();

    class TransportRadio final : public RFM69 {
    public:
        TransportRadio(uint8_t cs, uint8_t irq) : RFM69(cs,irq,true) {}
        volatile uint8_t control = 0;
    protected:
        void interruptHook(uint8_t value) override { control = value; }
    };
    TransportRadio radio_;
    // Applied transport profile; V3 keeps hardware AES disabled.
    uint8_t address_ = 0;
    uint8_t network_ = 0;
    uint8_t level_ = NODE_RADIO_MAX_POWER_LEVEL;
    uint8_t frequencyMsb_ = 0;
};

}  // namespace node
}  // namespace radiosensors
