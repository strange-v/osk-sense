#include <CommissioningService.h>
#include <RtcEntropy.h>
#include <TelemetryFrames.h>
// The release stack monitor is an empty RAII scope.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include <NodeRuntime.h>
#pragma GCC diagnostic pop
#include <unity.h>
#include <string.h>
#include <stdio.h>

namespace s = radiosensors::security;
namespace f = s::frames;
namespace n = radiosensors::node;
namespace p = radiosensors::protocol;
FakeEeprom EEPROM;
uint8_t userRow[32], serialNumber[10];
uint32_t fakeMillis=0;
namespace {
const uint8_t factory[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
const uint8_t salt[8]={16,17,18,19,20,21,22,23};
s::Context gateway;
osk::crypto::Cmac factoryMac;
bool entropyWorks=true, wrongComplete=false;
enum Mode { Normal, Drop, Floor, BadFloor, Challenge, BadAcks, AlwaysChallenge } mode;
unsigned confirms=0;
n::ButtonGesture gesture = n::ButtonGesture::None;
unsigned clockSleeps = 0;
void queue(s::Transport transport,const uint8_t* bytes,size_t size) {
    RFM69::incoming.push_back(Packet{transport,std::vector<uint8_t>(bytes,bytes+size)});
}
void ack(uint32_t counter,const s::Ack& value,bool corrupt=false,uint8_t control=0x80) {
    uint8_t wire[16]; size_t size;
    TEST_ASSERT_TRUE(s::sealAck(gateway.authentication,s::Transport{7,100,0x80},counter,value,wire,sizeof(wire),size));
    if (corrupt) wire[size-1]^=1;
    queue(s::Transport{7,100,control},wire,size);
}
void respond(const Packet& packet) {
    const auto& wire=packet.bytes;
    if (wire[0]==f::kJoinRequestHeader) {
        f::JoinRequest request;
        TEST_ASSERT_TRUE(f::openJoinRequest(factoryMac,packet.transport,wire.data(),wire.size(),request));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(serialNumber,request.identity.uid,10);
        uint32_t limit;
        TEST_ASSERT_TRUE(n::security_storage::decodeReserve(EEPROM.data+n::security_storage::kReserveA,limit));
        TEST_ASSERT_GREATER_THAN(static_cast<uint32_t>(request.identity.requestNonce),limit);
        f::JoinAccept accept;
        accept.identity=request.identity; memcpy(accept.salt,salt,8); accept.nodeId=7; accept.networkId=42;
        uint8_t reply[35]; TEST_ASSERT_TRUE(f::sealJoinAccept(factoryMac,s::Transport{0,100,0},accept,reply,35));
        queue(s::Transport{0,100,0},reply,35); return;
    }
    if (wire[0]==f::kJoinConfirmHeader) {
        ++confirms;
        n::security_storage::NetworkConfigStore<FakeEeprom> stored(EEPROM); n::security_storage::NetworkConfig config;
        TEST_ASSERT_TRUE(stored.load(config)); TEST_ASSERT_EQUAL_UINT8(1,static_cast<uint8_t>(config.state));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(salt,config.salt,8);
        f::JoinIdentity proof; TEST_ASSERT_TRUE(f::openJoinProof(gateway.authentication,packet.transport,wire.data(),wire.size(),salt,false,proof));
        if (wrongComplete) ++proof.requestNonce;
        uint8_t reply[25]; TEST_ASSERT_TRUE(f::sealJoinProof(gateway.authentication,s::Transport{7,100,0},proof,salt,true,reply,25));
        queue(s::Transport{7,100,0},reply,25); return;
    }
    std::vector<uint8_t> decoded=wire;
    uint32_t counter; uint8_t* payload; size_t size;
    TEST_ASSERT_TRUE(s::open(gateway,s::Direction::Node,7,packet.transport,decoded.data(),decoded.size(),counter,payload,size));
    TEST_ASSERT_EQUAL_UINT8(0x40,packet.transport.control);
    if (mode==Drop) return;
    s::Ack reply;
    if (mode==Floor || mode==BadFloor) {
        reply.hasCounterFloor=true; reply.counterFloor=counter+(mode==Floor ? 10 : 300);
        mode=Normal;
    } else if (mode==Challenge || mode==AlwaysChallenge) {
        reply.hasChallenge=true; for (uint8_t i=0;i<8;++i) reply.challenge[i]=i+70;
        if (mode==Challenge) mode=Normal;
    } else if (mode==BadAcks) {
        ack(counter,reply,true); ack(counter-1,reply); ack(counter,reply,false,0x90);
        mode=Normal;
    }
    ack(counter,reply);
}
struct Fixture {
    n::NodeRadio radio{4,7}; n::CommissioningService service{radio,6};
    uint8_t telemetry[p::kVoltageTelemetrySize];
    Fixture() {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::StartStatus::Ready),static_cast<uint8_t>(service.begin()));
        TEST_ASSERT_TRUE(service.advance()); TEST_ASSERT_TRUE(service.active());
        TEST_ASSERT_TRUE(p::encodeVoltageTelemetry(p::TelemetryPrefix{2500,15,-70},telemetry,sizeof(telemetry))==p::TelemetryCodecStatus::Ok);
        RFM69::outgoing.clear();
    }
    bool report(uint8_t attempts=3) { s::Ack reply; int8_t rssi; return service.sendReport(telemetry,sizeof(telemetry),attempts,reply,rssi); }
};
uint32_t counter(const Packet& packet) { return p::readUint32Le(packet.bytes.data()+1); }
struct SessionProfile {
    static constexpr uint16_t kProfileId = 6;
    static constexpr size_t kTelemetrySize = p::kVoltageTelemetrySize;
    unsigned applied = 0;
    void begin() {}
    void poll(uint32_t) {}
    bool reportDue(uint32_t) const { return false; }
    bool takeUrgentReport() { return false; }
    size_t encodeTelemetry(const p::TelemetryPrefix&, uint8_t*, size_t) { return 0; }
    void reportAcknowledged(uint32_t, uint16_t) {}
    void reportFailed(uint32_t, uint16_t) {}
    void commissioned() {}
    void applyCommand(const f::Command& command, f::CommandResult& result) {
        TEST_ASSERT_EQUAL_UINT16(42, command.commandId);
        ++applied;
        result.status = p::CommandStatus::Applied;
    }
};
struct SessionFixture : Fixture {
    SessionProfile profile;
    n::LowPowerClock clock;
    n::ProvisioningButton button{6};
    n::NodeRuntime<SessionProfile> runtime{profile, radio, service, clock, button};
    SessionFixture() { runtime.begin(); RFM69::outgoing.clear(); }
    void run() {
        gesture = n::ButtonGesture::ShortPress;
        runtime.runOnce();
        TEST_ASSERT_FALSE(RFM69::receiving);
    }
};
Packet commandReply(uint32_t ready, bool hasCommand = true) {
    uint8_t payload[11], wire[22]; size_t payloadSize = 0, size;
    f::Command command; command.commandId = 42; command.type = 1;
    if (hasCommand) TEST_ASSERT_TRUE(f::encodeCommand(command, payload, sizeof(payload), payloadSize));
    const s::Transport transport{7, 100, 0};
    TEST_ASSERT_TRUE(s::seal(gateway, s::Direction::Gateway, 7, transport, ready,
        hasCommand ? f::kCommandHeader : f::kNoCommandHeader,
        payload, payloadSize, wire, sizeof(wire), size));
    return Packet{transport, std::vector<uint8_t>(wire, wire + size)};
}
void acceptResult(const Packet& packet) {
    TEST_ASSERT_EQUAL_UINT8(f::kCommandResultHeader, packet.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x40, packet.transport.control);
    auto wire = packet.bytes;
    uint32_t value; uint8_t* payload; size_t size;
    TEST_ASSERT_TRUE(s::open(gateway, s::Direction::Node, 7, packet.transport,
        wire.data(), wire.size(), value, payload, size));
    f::CommandResult result;
    TEST_ASSERT_TRUE(f::decodeCommandResult(payload, size, result));
    TEST_ASSERT_EQUAL_UINT16(42, result.commandId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(p::CommandStatus::Applied), static_cast<uint8_t>(result.status));
    ack(value, s::Ack{});
}
}
namespace radiosensors { namespace node {
void LowPowerClock::begin() {}
uint32_t LowPowerClock::nowMs() const { return fakeMillis; }
void LowPowerClock::sleepUntilInterrupt() const { ++clockSleeps; }
void ProvisioningButton::begin() {}
ButtonGesture ProvisioningButton::takeGesture() {
    const auto value = gesture; gesture = ButtonGesture::None; return value;
}
uint16_t BatteryMonitor::readMillivolts() const { return 2500; }
bool collectRtcEntropy(uint8_t (&out)[8]) {
    if (!entropyWorks) return false;
    for (uint8_t i=0;i<8;++i) out[i]=i+40;
    return true;
}
} }
void setUp() {
    memset(EEPROM.data,0xFF,sizeof(EEPROM.data)); EEPROM.remaining=-1;
    for (uint8_t i=0;i<10;++i) serialNumber[i]=i;
    n::storage::FactoryCredentials credentials; memcpy(credentials.key,factory,16);
    TEST_ASSERT_TRUE(n::storage::encodeFactoryCredentials(credentials,userRow,32));
    fakeMillis=0; mode=Normal; entropyWorks=true; wrongComplete=false; confirms=0;
    gesture = n::ButtonGesture::None; clockSleeps = 0;
    RFM69::incoming.clear(); RFM69::outgoing.clear(); RFM69::receiving=false;
    RFM69::initialized=true; RFM69::onSend=respond; RFM69::onSleep=nullptr;
    osk::crypto::cmacInit(factoryMac,factory); s::initialize(gateway,factory,salt);
}
void tearDown() { RFM69::onSend=nullptr; RFM69::onSleep=nullptr; }
void test_real_service_pairs_then_boot_skips_reserved_counters() {
    Fixture x; TEST_ASSERT_EQUAL_UINT(1,confirms); TEST_ASSERT_TRUE(x.report());
    TEST_ASSERT_EQUAL_UINT8(0x60,RFM69::outgoing[0].bytes[0]); TEST_ASSERT_EQUAL_UINT32(1,counter(RFM69::outgoing[0]));
    n::NodeRadio radio(4,7); n::CommissioningService reboot(radio,6);
    TEST_ASSERT_EQUAL_UINT8(0,static_cast<uint8_t>(reboot.begin())); TEST_ASSERT_TRUE(reboot.active());
    s::Ack reply; int8_t rssi;
    TEST_ASSERT_TRUE(reboot.sendReport(x.telemetry,sizeof(x.telemetry),3,reply,rssi));
    TEST_ASSERT_EQUAL_UINT32(1024,counter(RFM69::outgoing.back()));
}
void test_retries_retain_exact_ciphertext_and_invalid_acks_are_ignored() {
    Fixture x; mode=Drop; TEST_ASSERT_FALSE(x.report(2)); TEST_ASSERT_EQUAL_UINT(2,RFM69::outgoing.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RFM69::outgoing[0].bytes.data(),RFM69::outgoing[1].bytes.data(),RFM69::outgoing[0].bytes.size());
    mode=BadAcks; TEST_ASSERT_TRUE(x.report()); TEST_ASSERT_EQUAL_UINT(3,RFM69::outgoing.size());
    TEST_ASSERT_EQUAL_UINT32(2,counter(RFM69::outgoing.back()));
}
void test_ack_is_copied_and_radio_sleeps_before_authentication() {
    Fixture x;
    s::Ack received; int8_t rssi = p::kNoDownlinkRssi;
    unsigned sleeps = 0;
    RFM69::onSend = [](const Packet&) {
        s::Ack reply; reply.commandPending = true;
        ack(42, reply, true);
        ack(42, reply);
    };
    RFM69::onSleep = [&]() {
        ++sleeps;
        TEST_ASSERT_FALSE(received.commandPending);
        // Authentication must use the captured payload and transport.
        memset(RFM69::DATA, 0, sizeof(RFM69::DATA));
        RFM69::DATALEN = 0; RFM69::TARGETID = 0; RFM69::SENDERID = 0;
    };
    const uint8_t frame[] = {0};
    TEST_ASSERT_TRUE(x.radio.sendAcknowledged(100, frame, sizeof(frame), 1,
                                            gateway.authentication, 42, received, rssi));
    TEST_ASSERT_TRUE(received.commandPending);
    TEST_ASSERT_EQUAL_UINT(2, sleeps);
    TEST_ASSERT_EQUAL_UINT(1, RFM69::outgoing.size());
    TEST_ASSERT_TRUE(RFM69::incoming.empty());
    TEST_ASSERT_FALSE(RFM69::receiving);
}
void test_invalid_ack_verification_does_not_extend_receive_deadline() {
    Fixture x;
    const uint8_t frame[] = {0};
    for (const uint32_t started : {uint32_t(100), UINT32_MAX - 20}) {
        fakeMillis = started;
        RFM69::incoming.clear(); RFM69::outgoing.clear();
        unsigned sleeps = 0;
        RFM69::onSend = [](const Packet&) {
            s::Ack reply;
            ack(42, reply, true);
            ack(42, reply);
        };
        RFM69::onSleep = [&]() {
            if (++sleeps == 1) fakeMillis += 40;
        };
        s::Ack received; int8_t rssi = p::kNoDownlinkRssi;
        TEST_ASSERT_FALSE(x.radio.sendAcknowledged(100, frame, sizeof(frame), 1,
                                                 gateway.authentication, 42, received, rssi));
        TEST_ASSERT_EQUAL_UINT(1, RFM69::outgoing.size());
        TEST_ASSERT_EQUAL_UINT(1, RFM69::incoming.size());
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(45, static_cast<uint32_t>(fakeMillis - started));
        TEST_ASSERT_FALSE(RFM69::receiving);
    }
}
void test_floor_commits_then_reseals_report_at_new_counter() {
    Fixture x; mode=Floor; TEST_ASSERT_TRUE(x.report()); TEST_ASSERT_EQUAL_UINT(2,RFM69::outgoing.size());
    TEST_ASSERT_EQUAL_UINT32(1,counter(RFM69::outgoing[0])); TEST_ASSERT_EQUAL_UINT32(11,counter(RFM69::outgoing[1]));
    uint32_t limit; TEST_ASSERT_TRUE(n::security_storage::decodeReserve(EEPROM.data+n::security_storage::kReserveB,limit));
    TEST_ASSERT_EQUAL_UINT32(1035,limit);
}
void test_out_of_range_floor_is_not_applied() {
    Fixture x; mode=BadFloor; TEST_ASSERT_FALSE(x.report());
    TEST_ASSERT_TRUE(x.report()); TEST_ASSERT_EQUAL_UINT32(2,counter(RFM69::outgoing.back()));
}
void test_activation_uses_fresh_counter_and_clear_authenticated_challenge() {
    Fixture x; mode=Challenge; TEST_ASSERT_TRUE(x.report()); TEST_ASSERT_EQUAL_UINT(2,RFM69::outgoing.size());
    const auto& activation=RFM69::outgoing[1];
    TEST_ASSERT_EQUAL_UINT8(0x6A,activation.bytes[0]); TEST_ASSERT_EQUAL_UINT32(2,counter(activation));
    for (uint8_t i=0;i<8;++i) TEST_ASSERT_EQUAL_UINT8(i+70,activation.bytes[i+5]);
    TEST_ASSERT_FALSE(x.service.needsActivation());
    mode=AlwaysChallenge; TEST_ASSERT_FALSE(x.report()); TEST_ASSERT_TRUE(x.service.needsActivation());
    TEST_ASSERT_EQUAL_UINT(5,RFM69::outgoing.size());
}
void test_result_challenge_defers_result_and_next_report_activates() {
    Fixture x; mode=Challenge;
    const uint8_t result[]={1,0,0}; TEST_ASSERT_FALSE(x.service.sendResult(result,sizeof(result)));
    TEST_ASSERT_TRUE(x.service.needsActivation()); TEST_ASSERT_TRUE(x.report());
    TEST_ASSERT_EQUAL_UINT8(0x6A,RFM69::outgoing.back().bytes[0]); TEST_ASSERT_FALSE(x.service.needsActivation());
}
void test_storage_and_entropy_failure_prevent_join_transmission() {
    n::NodeRadio radio(4,7); n::CommissioningService service(radio,6); service.begin();
    entropyWorks=false; TEST_ASSERT_FALSE(service.advance()); TEST_ASSERT_TRUE(RFM69::outgoing.empty());
    entropyWorks=true; EEPROM.remaining=0;
    TEST_ASSERT_FALSE(service.advance()); TEST_ASSERT_TRUE(RFM69::outgoing.empty());
}
void test_invalid_complete_preserves_provisional_salt_across_boot() {
    wrongComplete=true; n::NodeRadio radio(4,7); n::CommissioningService service(radio,6); service.begin();
    TEST_ASSERT_FALSE(service.advance()); TEST_ASSERT_FALSE(service.active()); TEST_ASSERT_TRUE(service.provisional());
    TEST_ASSERT_EQUAL_UINT(3,confirms);
    wrongComplete=false; n::NodeRadio freshRadio(4,7); n::CommissioningService reboot(freshRadio,6); reboot.begin();
    TEST_ASSERT_TRUE(reboot.advance()); TEST_ASSERT_TRUE(reboot.active()); TEST_ASSERT_EQUAL_UINT(4,confirms);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(salt,reboot.config().salt,8);
}
void test_missing_factory_key_and_lost_counter_reserves_fail_closed() {
    Fixture x; memset(EEPROM.data+n::security_storage::kReserveA,0xFF,6); memset(EEPROM.data+n::security_storage::kReserveB,0xFF,6);
    n::NodeRadio radio(4,7); n::CommissioningService reboot(radio,6);
    TEST_ASSERT_EQUAL_UINT8(0,static_cast<uint8_t>(reboot.begin())); TEST_ASSERT_FALSE(reboot.active()); TEST_ASSERT_FALSE(reboot.provisional());
    memset(userRow,0xFF,32);
    n::CommissioningService invalid(radio,6); TEST_ASSERT_EQUAL_UINT8(1,static_cast<uint8_t>(invalid.begin()));
}
void test_command_session_floor_reseals_ready_and_accepts_no_command() {
    SessionFixture x;
    unsigned readyCount = 0;
    RFM69::onSend = [&](const Packet& packet) {
        TEST_ASSERT_EQUAL_UINT8(f::kCommandReadyHeader, packet.bytes[0]);
        TEST_ASSERT_EQUAL_UINT8(0, packet.transport.control);
        if (++readyCount == 1) {
            s::Ack reply; reply.hasCounterFloor = true; reply.counterFloor = counter(packet) + 10;
            ack(counter(packet), reply);
        } else {
            uint32_t limit;
            TEST_ASSERT_TRUE(n::security_storage::decodeReserve(EEPROM.data + n::security_storage::kReserveA, limit));
            TEST_ASSERT_EQUAL_UINT32(counter(packet) + 1024, limit);
            RFM69::incoming.push_back(commandReply(counter(packet), false));
        }
    };
    x.run();
    TEST_ASSERT_EQUAL_UINT(2, readyCount);
    TEST_ASSERT_EQUAL_UINT32(counter(RFM69::outgoing[0]) + 10, counter(RFM69::outgoing[1]));
    TEST_ASSERT_EQUAL_UINT(0, x.profile.applied);
    TEST_ASSERT_EQUAL_UINT(1, clockSleeps);
}
void test_command_session_challenge_aborts_and_next_report_activates() {
    SessionFixture x;
    RFM69::onSend = [](const Packet& packet) {
        TEST_ASSERT_EQUAL_UINT8(f::kCommandReadyHeader, packet.bytes[0]);
        s::Ack reply; reply.hasChallenge = true;
        for (uint8_t i = 0; i < 8; ++i) reply.challenge[i] = i + 70;
        ack(counter(packet), reply);
    };
    x.run();
    TEST_ASSERT_EQUAL_UINT(1, RFM69::outgoing.size());
    TEST_ASSERT_EQUAL_UINT(0, x.profile.applied);
    TEST_ASSERT_TRUE(x.service.needsActivation());
    const uint32_t ready = counter(RFM69::outgoing[0]);
    RFM69::onSend = respond;
    TEST_ASSERT_TRUE(x.report());
    TEST_ASSERT_EQUAL_UINT8(s::kActivationHeader, RFM69::outgoing.back().bytes[0]);
    TEST_ASSERT_EQUAL_UINT32(ready + 1, counter(RFM69::outgoing.back()));
    for (uint8_t i = 0; i < 8; ++i) TEST_ASSERT_EQUAL_UINT8(i + 70, RFM69::outgoing.back().bytes[i + 5]);
    TEST_ASSERT_FALSE(x.service.needsActivation());
}
void test_command_session_timeout_retries_identical_ready_and_sleeps() {
    SessionFixture x;
    RFM69::onSend = [](const Packet&) {};
    const uint32_t started = fakeMillis;
    x.run();
    TEST_ASSERT_EQUAL_UINT(3, RFM69::outgoing.size());
    for (size_t i = 1; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT(RFM69::outgoing[0].bytes.size(), RFM69::outgoing[i].bytes.size());
        TEST_ASSERT_EQUAL_HEX8_ARRAY(RFM69::outgoing[0].bytes.data(), RFM69::outgoing[i].bytes.data(), RFM69::outgoing[0].bytes.size());
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(750, fakeMillis - started);
    TEST_ASSERT_EQUAL_UINT(0, x.profile.applied);
    TEST_ASSERT_EQUAL_UINT(1, clockSleeps);
}
void test_command_session_repeated_cached_reply_applies_once_and_retries_result() {
    SessionFixture x;
    Packet cached = commandReply(1024);
    unsigned readyCount = 0, resultCount = 0;
    RFM69::onSend = [&](const Packet& packet) {
        if (packet.bytes[0] == f::kCommandReadyHeader) {
            ++readyCount;
            if (readyCount == 1) { cached = commandReply(counter(packet)); return; }
            TEST_ASSERT_EQUAL_UINT32(counter(RFM69::outgoing[0]), counter(packet));
            RFM69::incoming.push_back(cached);
            RFM69::incoming.push_back(cached);
        } else if (++resultCount == 2) {
            acceptResult(packet);
        }
    };
    x.run();
    TEST_ASSERT_EQUAL_UINT(2, readyCount);
    TEST_ASSERT_EQUAL_UINT(2, resultCount);
    TEST_ASSERT_EQUAL_UINT(1, x.profile.applied);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RFM69::outgoing[2].bytes.data(), RFM69::outgoing[3].bytes.data(), RFM69::outgoing[2].bytes.size());
}
void test_command_session_rejects_reply_from_previous_session() {
    SessionFixture x;
    Packet old = commandReply(1024);
    RFM69::onSend = [&](const Packet& packet) {
        old = commandReply(counter(packet), false);
        RFM69::incoming.push_back(old);
    };
    x.run();
    const uint32_t previous = counter(RFM69::outgoing[0]);
    RFM69::outgoing.clear();
    RFM69::onSend = [&](const Packet& packet) {
        TEST_ASSERT_EQUAL_UINT8(f::kCommandReadyHeader, packet.bytes[0]);
        RFM69::incoming.push_back(old);
    };
    x.run();
    TEST_ASSERT_EQUAL_UINT(3, RFM69::outgoing.size());
    TEST_ASSERT_EQUAL_UINT32(previous + 1, counter(RFM69::outgoing[0]));
    TEST_ASSERT_EQUAL_UINT(0, x.profile.applied);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    UNITY_BEGIN();
    RUN_TEST(test_real_service_pairs_then_boot_skips_reserved_counters);
    RUN_TEST(test_retries_retain_exact_ciphertext_and_invalid_acks_are_ignored);
    RUN_TEST(test_ack_is_copied_and_radio_sleeps_before_authentication);
    RUN_TEST(test_invalid_ack_verification_does_not_extend_receive_deadline);
    RUN_TEST(test_floor_commits_then_reseals_report_at_new_counter);
    RUN_TEST(test_out_of_range_floor_is_not_applied);
    RUN_TEST(test_activation_uses_fresh_counter_and_clear_authenticated_challenge);
    RUN_TEST(test_result_challenge_defers_result_and_next_report_activates);
    RUN_TEST(test_storage_and_entropy_failure_prevent_join_transmission);
    RUN_TEST(test_invalid_complete_preserves_provisional_salt_across_boot);
    RUN_TEST(test_missing_factory_key_and_lost_counter_reserves_fail_closed);
    RUN_TEST(test_command_session_floor_reseals_ready_and_accepts_no_command);
    RUN_TEST(test_command_session_challenge_aborts_and_next_report_activates);
    RUN_TEST(test_command_session_timeout_retries_identical_ready_and_sleeps);
    RUN_TEST(test_command_session_repeated_cached_reply_applies_once_and_retries_result);
    RUN_TEST(test_command_session_rejects_reply_from_previous_session);
    return UNITY_END();
}
