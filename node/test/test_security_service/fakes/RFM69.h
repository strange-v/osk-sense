#pragma once
#include <RadioSecurity.h>
#include <functional>
#include <vector>
#include <deque>
#include <string.h>
#include "RFM69registers.h"
#define RF69_868MHZ 86
struct Packet {
    radiosensors::security::Transport transport;
    std::vector<uint8_t> bytes;
};
class RFM69 {
public:
    inline static uint16_t TARGETID=0, SENDERID=0;
    inline static uint8_t DATA[61]{}, DATALEN=0;
    inline static uint8_t registers[256]{};
    inline static std::deque<Packet> incoming;
    inline static std::vector<Packet> outgoing;
    inline static std::function<void(const Packet&)> onSend;
    inline static bool initialized=true, receiving=false;
    RFM69(uint8_t,uint8_t,bool) {}
    virtual ~RFM69() = default;
    bool initialize(uint8_t,uint8_t address,uint8_t network) {
        address_=address; registers[REG_FRFMSB]=0xD9; registers[REG_SYNCVALUE2]=network;
        registers[REG_PACKETCONFIG2]=0; return initialized;
    }
    void setHighPower(bool) {}
    void setAddress(uint8_t address) { address_=address; }
    void setNetwork(uint8_t network) { registers[REG_SYNCVALUE2]=network; }
    void encrypt(const char*) { registers[REG_PACKETCONFIG2]=0; }
    void setPowerLevel(uint8_t) {}
    uint8_t readReg(uint8_t i) { return registers[i]; }
    int16_t readRSSI() { return -71; }
    void send(uint8_t target,const uint8_t* bytes,uint8_t size,bool ack) {
        Packet packet{{target,address_,static_cast<uint8_t>(ack ? 0x40 : 0)},std::vector<uint8_t>(bytes,bytes+size)};
        outgoing.push_back(packet); receiving=false;
        if (onSend) onSend(packet);
    }
    bool receiveDone() {
        if (!receiving) { receiving=true; return false; }
        if (incoming.empty()) return false;
        Packet packet=incoming.front(); incoming.pop_front();
        TARGETID=packet.transport.target; SENDERID=packet.transport.sender;
        DATALEN=packet.bytes.size(); memcpy(DATA,packet.bytes.data(),DATALEN);
        control_=packet.transport.control; interruptHook(control_);
        receiving=false;
        return true;
    }
    bool ACKReceived(uint8_t sender) { return receiveDone() && SENDERID==sender && (control_&0x80); }
    void sleep() { receiving=false; }
protected:
    virtual void interruptHook(uint8_t) {}
private:
    uint8_t address_=0, control_=0;
};
