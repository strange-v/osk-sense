#include <RegistryRadio.h>
#include <unity.h>
#include <string.h>
#include <vector>

namespace s = radiosensors::security;
namespace f = s::frames;
namespace p = s::pairing;
namespace r = radiosensors::replay;
namespace n = radiosensors::registry;
namespace {
struct Memory : r::BlobStorage {
    std::vector<uint8_t> bytes;
    unsigned writes = 0;
    bool fail = false;
    r::ReadStatus read(uint8_t* out, size_t capacity, size_t& size) override {
        size = 0;
        if (bytes.empty()) return r::ReadStatus::Missing;
        if (bytes.size() > capacity) return r::ReadStatus::Invalid;
        size = bytes.size(); memcpy(out, bytes.data(), size); return r::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        ++writes;
        if (fail) return false;
        bytes.assign(data, data + size); return true;
    }
};
struct Random : r::RandomSource {
    unsigned calls = 0;
    bool fill(uint8_t* out, size_t size) override {
        ++calls;
        for (size_t i = 0; i < size; ++i) out[i] = calls + i;
        return true;
    }
};
const uint8_t report[] = {0, 33, 12, 0};
struct Fixture {
    Memory registryMemory, boundMemory;
    Random random;
    n::NodeRegistry nodes;
    n::AtomicRegistryStore store{registryMemory};
    r::Store bounds{boundMemory};
    r::Guard guard{bounds,random};
    n::PairingAdapter pairing{nodes,store,bounds,guard,random};
    n::RadioAdapter radio{pairing,guard};
    s::Context node;
    uint8_t salt[8]{};
    Fixture() { store.load(nodes); bounds.load(); guard.restart(); pair(1); }
    void pair(uint8_t uid) {
        const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        osk::crypto::Cmac mac; osk::crypto::cmacInit(mac,key);
        f::JoinRequest request; request.identity.uid[0] = uid;
        request.identity.requestNonce = 0xFEDCBA9812345678ULL;
        request.profileId = 6; request.firmware = {1,2,3}; request.maxPowerLevel = 31;
        uint8_t wire[33], accepted[35], proof[27], complete[25];
        TEST_ASSERT_TRUE(f::sealJoinRequest(mac,{100,0,0},request,wire,sizeof(wire)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(p::Status::Accept),
            static_cast<uint8_t>(pairing.request(mac,request.identity.uid,{100,0,0},wire,sizeof(wire),
                                                 123,{0,100,0},accepted,sizeof(accepted))));
        f::JoinAccept accept;
        TEST_ASSERT_TRUE(f::openJoinAccept(mac,{0,100,0},accepted,sizeof(accepted),accept));
        TEST_ASSERT_EQUAL_UINT8(1,accept.nodeId);
        memcpy(salt,accept.salt,sizeof(salt)); s::initialize(node,key,salt);
        TEST_ASSERT_TRUE(f::sealJoinProof(node.authentication,{100,1,0},request.identity,salt,false,proof,sizeof(proof)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(p::Status::Complete),
            static_cast<uint8_t>(pairing.confirm(1,{100,1,0},proof,sizeof(proof),complete,sizeof(complete))));
        f::JoinIdentity confirmed;
        TEST_ASSERT_TRUE(f::openJoinProof(node.authentication,{1,100,0},complete,sizeof(complete),salt,true,confirmed));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(request.identity.uid,confirmed.uid,sizeof(confirmed.uid));
    }
    std::vector<uint8_t> seal(uint32_t counter, uint8_t header = 0x60,
                              const uint8_t* payload = report, size_t size = sizeof(report),
                              const uint8_t* challenge = nullptr, uint8_t control = 0x40) {
        uint8_t wire[s::kMaxWireSize]; size_t wireSize = 0;
        TEST_ASSERT_TRUE(s::seal(node,s::Direction::Node,1,{100,1,control},counter,
                                 header,payload,size,wire,sizeof(wire),wireSize,challenge));
        return {wire,wire+wireSize};
    }
    n::OpenedFrame open(std::vector<uint8_t> wire, r::Action action,
                         uint8_t control = 0x40, uint64_t now = 0) {
        n::OpenedFrame opened;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
            static_cast<uint8_t>(radio.receive({100,1,control},wire.data(),wire.size(),now,true,true,opened)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(action),static_cast<uint8_t>(opened.decision.action));
        return opened;
    }
    void restart() { store.load(nodes); bounds.load(); guard.restart(); radio.restart(); }
};
}
void setUp() {}
void tearDown() {}

void test_pair_receive_and_authenticated_ack() {
    Fixture x; const auto wire = x.seal(1024);
    const auto accepted = x.open(wire,r::Action::Accept);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(report,accepted.payload,sizeof(report));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x.salt,accepted.salt,sizeof(x.salt));
    const auto writes = x.boundMemory.writes;
    x.open(wire,r::Action::Duplicate); TEST_ASSERT_EQUAL_UINT(writes,x.boundMemory.writes);
    s::Ack ack; ack.commandPending = true; ack.hasPowerTarget = true; ack.powerTarget = 7;
    uint8_t bytes[16]; size_t size = 0;
    TEST_ASSERT_TRUE(s::sealAck(accepted.mac,{1,100,0x80},1024,ack,bytes,sizeof(bytes),size));
    s::Ack verified;
    TEST_ASSERT_TRUE(s::openAck(x.node.authentication,{1,100,0x80},1024,bytes,size,verified));
    TEST_ASSERT_TRUE(verified.commandPending); TEST_ASSERT_EQUAL_UINT8(7,verified.powerTarget);
    TEST_ASSERT_FALSE(s::openAck(x.node.authentication,{1,100,0x80},1025,bytes,size,verified));
}
void test_tampering_never_decrypts_or_advances_counter() {
    Fixture x; const auto valid = x.seal(1024); const auto writes = x.boundMemory.writes;
    for (size_t i = 0; i < valid.size(); ++i) {
        auto wire = valid; wire[i] ^= 1; const auto before = wire; n::OpenedFrame opened;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::FailedTag),
            static_cast<uint8_t>(x.radio.receive({100,1,0x40},wire.data(),wire.size(),0,true,true,opened)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(before.data(),wire.data(),wire.size());
    }
    auto wire = valid; n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::FailedTag),
        static_cast<uint8_t>(x.radio.receive({100,1,0},wire.data(),wire.size(),0,true,true,opened)));
    TEST_ASSERT_EQUAL_UINT(writes,x.boundMemory.writes); x.open(valid,r::Action::Accept);
}
void test_queue_backpressure_does_not_consume_fresh_counter() {
    Fixture x; auto wire = x.seal(1024); n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Busy),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},wire.data(),wire.size(),0,false,true,opened)));
    x.open(x.seal(1024),r::Action::Accept);
    auto ready = x.seal(1025,f::kCommandReadyHeader,nullptr,0,nullptr,0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Busy),
        static_cast<uint8_t>(x.radio.receive({100,1,0},ready.data(),ready.size(),0,true,false,opened)));
    x.open(x.seal(1025,f::kCommandReadyHeader,nullptr,0,nullptr,0),r::Action::Accept,0);
}
void test_accepted_reports_and_results_are_reacked_when_consumer_queues_are_full() {
    Fixture x;
    const auto reportWire = x.seal(1024);
    x.open(reportWire,r::Action::Accept);
    const auto reportWrites = x.boundMemory.writes;
    auto repeated = reportWire; n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},repeated.data(),repeated.size(),0,false,false,opened)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::Action::Duplicate),static_cast<uint8_t>(opened.decision.action));
    TEST_ASSERT_FALSE(opened.retryCommandReady);
    TEST_ASSERT_EQUAL_UINT(reportWrites,x.boundMemory.writes);
    uint8_t ack[16]; size_t ackSize = 0; s::Ack verified;
    TEST_ASSERT_TRUE(s::sealAck(opened.mac,{1,100,0x80},opened.counter,{},ack,sizeof(ack),ackSize));
    TEST_ASSERT_TRUE(s::openAck(x.node.authentication,{1,100,0x80},1024,ack,ackSize,verified));

    f::CommandResult result; result.commandId = 42;
    uint8_t payload[11]; size_t payloadSize = 0;
    TEST_ASSERT_TRUE(f::encodeCommandResult(result,payload,sizeof(payload),payloadSize));
    const auto resultWire = x.seal(1025,f::kCommandResultHeader,payload,payloadSize);
    auto fresh = resultWire;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Busy),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},fresh.data(),fresh.size(),0,true,false,opened)));
    TEST_ASSERT_EQUAL_UINT(reportWrites,x.boundMemory.writes);
    x.open(resultWire,r::Action::Accept);
    const auto resultWrites = x.boundMemory.writes;
    repeated = resultWire;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},repeated.data(),repeated.size(),0,false,false,opened)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::Action::Duplicate),static_cast<uint8_t>(opened.decision.action));
    TEST_ASSERT_FALSE(opened.retryCommandReady);
    TEST_ASSERT_EQUAL_UINT(resultWrites,x.boundMemory.writes);
    TEST_ASSERT_TRUE(s::sealAck(opened.mac,{1,100,0x80},opened.counter,{},ack,sizeof(ack),ackSize));
    TEST_ASSERT_TRUE(s::openAck(x.node.authentication,{1,100,0x80},1025,ack,ackSize,verified));

    repeated = resultWire; repeated.back() ^= 1;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::FailedTag),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},repeated.data(),repeated.size(),0,false,false,opened)));
    TEST_ASSERT_EQUAL_UINT(resultWrites,x.boundMemory.writes);
}
void test_restart_counter_floor_is_authenticated_and_reserves_before_accept() {
    Fixture x; x.open(x.seal(1024),r::Action::Accept); x.restart();
    const auto floor = x.open(x.seal(1024),r::Action::CounterFloor);
    TEST_ASSERT_EQUAL_UINT32(1280,floor.decision.floor);
    s::Ack ack; ack.hasCounterFloor = true; ack.counterFloor = floor.decision.floor;
    uint8_t wire[16]; size_t size = 0;
    TEST_ASSERT_TRUE(s::sealAck(floor.mac,{1,100,0x80},1024,ack,wire,sizeof(wire),size));
    s::Ack verified;
    TEST_ASSERT_TRUE(s::openAck(x.node.authentication,{1,100,0x80},1024,wire,size,verified));
    TEST_ASSERT_EQUAL_UINT32(1280,verified.counterFloor);
    x.boundMemory.fail = true; auto next = x.seal(1280); n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::StorageError),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},next.data(),next.size(),0,true,true,opened)));
    x.boundMemory.fail = false; x.restart(); x.open(x.seal(1280),r::Action::Accept);
}
void test_backup_restore_requires_fresh_activation_challenge() {
    Fixture x; x.open(x.seal(1024),r::Action::Accept);
    x.boundMemory.bytes.clear(); x.restart();
    const auto first = x.open(x.seal(1024),r::Action::Challenge);
    const auto ready = x.open(x.seal(1025,f::kCommandReadyHeader,nullptr,0,nullptr,0),r::Action::Challenge,0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first.decision.challenge,ready.decision.challenge,8);
    uint8_t reply[22]; size_t replySize = 0;
    TEST_ASSERT_FALSE(x.radio.reply(1,x.salt,1025,f::kNoCommandHeader,nullptr,0,reply,sizeof(reply),replySize));
    x.restart(); const auto fresh = x.open(x.seal(1026),r::Action::Challenge);
    TEST_ASSERT_FALSE(memcmp(first.decision.challenge,fresh.decision.challenge,8) == 0);
    x.open(x.seal(1027,s::kActivationHeader,report,sizeof(report),first.decision.challenge),r::Action::Challenge);
    const auto activation = x.seal(1028,s::kActivationHeader,report,sizeof(report),fresh.decision.challenge);
    x.open(activation,r::Action::Accept); const auto writes = x.boundMemory.writes;
    x.open(activation,r::Action::Duplicate); TEST_ASSERT_EQUAL_UINT(writes,x.boundMemory.writes);
    x.restart(); x.open(activation,r::Action::CounterFloor);
}
void test_command_replies_are_immutable_and_bound_to_ready_counter() {
    Fixture x; const auto ready = x.seal(1024,f::kCommandReadyHeader,nullptr,0,nullptr,0);
    x.open(ready,r::Action::Accept,0);
    f::Command command; command.commandId = 42;
    command.type = static_cast<uint8_t>(radiosensors::protocol::CommandType::ReadInfo);
    uint8_t payload[11]; size_t payloadSize = 0;
    TEST_ASSERT_TRUE(f::encodeCommand(command,payload,sizeof(payload),payloadSize));
    uint8_t reply[22], repeated[22]; size_t size = 0, repeatedSize = 0;
    TEST_ASSERT_TRUE(x.radio.reply(1,x.salt,1024,f::kCommandHeader,payload,payloadSize,reply,sizeof(reply),size));
    TEST_ASSERT_TRUE(x.radio.reply(1,x.salt,1024,f::kNoCommandHeader,nullptr,0,repeated,sizeof(repeated),repeatedSize));
    TEST_ASSERT_EQUAL_UINT(size,repeatedSize); TEST_ASSERT_EQUAL_HEX8_ARRAY(reply,repeated,size);
    auto copy = ready; n::OpenedFrame duplicate;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
        static_cast<uint8_t>(x.radio.receive({100,1,0},copy.data(),copy.size(),0,true,false,duplicate)));
    TEST_ASSERT_EQUAL_UINT(size,duplicate.replySize); TEST_ASSERT_EQUAL_HEX8_ARRAY(reply,duplicate.reply,size);
    bool hasCommand = false; f::Command decoded;
    memcpy(repeated,reply,size);
    TEST_ASSERT_TRUE(f::openCommandReply(x.node,1,{1,100,0},1024,repeated,size,decoded,hasCommand));
    TEST_ASSERT_TRUE(hasCommand); TEST_ASSERT_EQUAL_UINT16(42,decoded.commandId);
    memcpy(repeated,reply,size);
    TEST_ASSERT_FALSE(f::openCommandReply(x.node,1,{1,100,0},1025,repeated,size,decoded,hasCommand));
    x.restart(); x.open(ready,r::Action::CounterFloor,0);
    TEST_ASSERT_FALSE(x.radio.reply(1,x.salt,1024,f::kNoCommandHeader,nullptr,0,repeated,sizeof(repeated),repeatedSize));
}
void test_ready_without_reply_can_retry_consumer_until_reply_is_cached() {
    Fixture x; const auto ready = x.seal(1024,f::kCommandReadyHeader,nullptr,0,nullptr,0);
    const auto first = x.open(ready,r::Action::Accept,0);
    TEST_ASSERT_FALSE(first.retryCommandReady);
    const auto writes = x.boundMemory.writes;
    const auto duplicate = x.open(ready,r::Action::Duplicate,0);
    TEST_ASSERT_TRUE(duplicate.retryCommandReady);
    TEST_ASSERT_EQUAL_UINT(0,duplicate.replySize);
    TEST_ASSERT_EQUAL_UINT(writes,x.boundMemory.writes);

    auto blocked = ready; n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Busy),
        static_cast<uint8_t>(x.radio.receive({100,1,0},blocked.data(),blocked.size(),0,true,false,opened)));
    TEST_ASSERT_FALSE(opened.retryCommandReady);
    TEST_ASSERT_TRUE(x.open(ready,r::Action::Duplicate,0).retryCommandReady);

    uint8_t reply[22]; size_t replySize = 0;
    TEST_ASSERT_TRUE(x.radio.reply(1,x.salt,1024,f::kNoCommandHeader,nullptr,0,reply,sizeof(reply),replySize));
    auto repeated = ready;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
        static_cast<uint8_t>(x.radio.receive({100,1,0},repeated.data(),repeated.size(),0,true,false,opened)));
    TEST_ASSERT_FALSE(opened.retryCommandReady);
    TEST_ASSERT_EQUAL_UINT(replySize,opened.replySize);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reply,opened.reply,replySize);
    TEST_ASSERT_FALSE(x.open(x.seal(1025),r::Action::Accept).retryCommandReady);
    TEST_ASSERT_FALSE(x.open(x.seal(1025),r::Action::Duplicate).retryCommandReady);
}
void test_result_clears_reply_and_stale_pairing_work_is_rejected() {
    Fixture x; x.open(x.seal(1024,f::kCommandReadyHeader,nullptr,0,nullptr,0),r::Action::Accept,0);
    uint8_t reply[22]; size_t replySize = 0;
    TEST_ASSERT_TRUE(x.radio.reply(1,x.salt,1024,f::kNoCommandHeader,nullptr,0,reply,sizeof(reply),replySize));
    f::CommandResult result; result.commandId = 42;
    uint8_t payload[11]; size_t size = 0;
    TEST_ASSERT_TRUE(f::encodeCommandResult(result,payload,sizeof(payload),size));
    x.open(x.seal(1025,f::kCommandResultHeader,payload,size),r::Action::Accept);
    TEST_ASSERT_FALSE(x.radio.reply(1,x.salt,1024,f::kNoCommandHeader,nullptr,0,reply,sizeof(reply),replySize));
    uint8_t oldSalt[8]; memcpy(oldSalt,x.salt,sizeof(oldSalt)); const auto oldWire = x.seal(1026);
    TEST_ASSERT_TRUE(x.nodes.remove(1)); TEST_ASSERT_TRUE(x.store.save(x.nodes)); x.pair(2);
    x.open(x.seal(1024,f::kCommandReadyHeader,nullptr,0,nullptr,0),r::Action::Accept,0);
    TEST_ASSERT_FALSE(x.radio.reply(1,oldSalt,1024,f::kNoCommandHeader,nullptr,0,reply,sizeof(reply),replySize));
    auto old = oldWire; n::OpenedFrame opened;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::FailedTag),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},old.data(),old.size(),0,true,true,opened)));
}
void test_invalid_authenticated_payload_does_not_consume_counter() {
    Fixture x; const uint8_t shortReport[3]{}; n::OpenedFrame opened;
    auto wire = x.seal(1024,0x60,shortReport,sizeof(shortReport));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::InvalidFrame),
        static_cast<uint8_t>(x.radio.receive({100,1,0x40},wire.data(),wire.size(),0,true,true,opened)));
    x.open(x.seal(1024),r::Action::Accept);
}
void test_history_memory_is_bounded_and_released_for_removed_or_disabled_slots() {
    Fixture x;
    n::NodeRecord records[r::kNodeSlots];
    for (size_t slot = 0; slot < r::kNodeSlots; ++slot) {
        records[slot] = x.nodes.records()[0];
        records[slot].nodeId = static_cast<uint8_t>(slot + 1);
        records[slot].deviceUid[0] = records[slot].nodeId;
        records[slot].replaySlot = static_cast<uint8_t>(slot);
        p::Record pairing; uint32_t generation = 0;
        TEST_ASSERT_TRUE(p::decode(records[slot].pairing,sizeof(records[slot].pairing),pairing,generation));
        memcpy(pairing.request + 1,records[slot].deviceUid,sizeof(records[slot].deviceUid));
        memcpy(pairing.accept + 1,records[slot].deviceUid,sizeof(records[slot].deviceUid));
        pairing.accept[27] = records[slot].nodeId;
        TEST_ASSERT_TRUE(p::encode(pairing,generation,records[slot].pairing,sizeof(records[slot].pairing)));
        if (slot != 0) TEST_ASSERT_TRUE(x.guard.initializeFreshKeys(slot));
    }
    TEST_ASSERT_TRUE(x.nodes.restore(records,r::kNodeSlots));
    TEST_ASSERT_TRUE(x.store.save(x.nodes));
    for (uint8_t nodeId = 1; nodeId <= r::kNodeSlots; ++nodeId) {
        uint8_t wire[s::kMaxWireSize]; size_t size = 0; n::OpenedFrame opened;
        TEST_ASSERT_TRUE(s::seal(x.node,s::Direction::Node,nodeId,{100,nodeId,0x40},1024,
                                 0x60,report,sizeof(report),wire,sizeof(wire),size));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::ReceiveStatus::Ok),
            static_cast<uint8_t>(x.radio.receive({100,nodeId,0x40},wire,size,0,true,true,opened)));
    }
    TEST_ASSERT_EQUAL_UINT(r::kNodeSlots * sizeof(r::AcceptanceHistory),x.radio.historyBytes());
    TEST_ASSERT_LESS_OR_EQUAL_UINT(38400,x.radio.historyBytes());
    TEST_ASSERT_TRUE(x.nodes.remove(1));
    TEST_ASSERT_TRUE(x.nodes.disable(2));
    TEST_ASSERT_TRUE(x.store.save(x.nodes));
    x.radio.retainActiveSlots(x.nodes);
    TEST_ASSERT_EQUAL_UINT(62 * sizeof(r::AcceptanceHistory),x.radio.historyBytes());
    x.radio.retainActiveSlots(x.nodes);
    TEST_ASSERT_EQUAL_UINT(62 * sizeof(r::AcceptanceHistory),x.radio.historyBytes());
    TEST_ASSERT_TRUE(x.nodes.restore(nullptr,0)); TEST_ASSERT_TRUE(x.store.save(x.nodes));
    x.radio.retainActiveSlots(x.nodes);
    TEST_ASSERT_EQUAL_UINT(0,x.radio.historyBytes());
    x.radio.restart(); TEST_ASSERT_EQUAL_UINT(0,x.radio.historyBytes());
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pair_receive_and_authenticated_ack);
    RUN_TEST(test_tampering_never_decrypts_or_advances_counter);
    RUN_TEST(test_queue_backpressure_does_not_consume_fresh_counter);
    RUN_TEST(test_accepted_reports_and_results_are_reacked_when_consumer_queues_are_full);
    RUN_TEST(test_restart_counter_floor_is_authenticated_and_reserves_before_accept);
    RUN_TEST(test_backup_restore_requires_fresh_activation_challenge);
    RUN_TEST(test_command_replies_are_immutable_and_bound_to_ready_counter);
    RUN_TEST(test_ready_without_reply_can_retry_consumer_until_reply_is_cached);
    RUN_TEST(test_result_clears_reply_and_stale_pairing_work_is_rejected);
    RUN_TEST(test_invalid_authenticated_payload_does_not_consume_counter);
    RUN_TEST(test_history_memory_is_bounded_and_released_for_removed_or_disabled_slots);
    return UNITY_END();
}
