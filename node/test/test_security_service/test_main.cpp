#include <CommissioningService.h>
#include <RtcEntropy.h>
#include <TelemetryFrames.h>
#include <unity.h>
#include <string.h>

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
}
namespace radiosensors { namespace node {
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
    RFM69::incoming.clear(); RFM69::outgoing.clear(); RFM69::receiving=false;
    RFM69::initialized=true; RFM69::onSend=respond;
    osk::crypto::cmacInit(factoryMac,factory); s::initialize(gateway,factory,salt);
}
void tearDown() { RFM69::onSend=nullptr; }
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
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_real_service_pairs_then_boot_skips_reserved_counters);
    RUN_TEST(test_retries_retain_exact_ciphertext_and_invalid_acks_are_ignored);
    RUN_TEST(test_floor_commits_then_reseals_report_at_new_counter);
    RUN_TEST(test_out_of_range_floor_is_not_applied);
    RUN_TEST(test_activation_uses_fresh_counter_and_clear_authenticated_challenge);
    RUN_TEST(test_result_challenge_defers_result_and_next_report_activates);
    RUN_TEST(test_storage_and_entropy_failure_prevent_join_transmission);
    RUN_TEST(test_invalid_complete_preserves_provisional_salt_across_boot);
    RUN_TEST(test_missing_factory_key_and_lost_counter_reserves_fail_closed);
    return UNITY_END();
}
