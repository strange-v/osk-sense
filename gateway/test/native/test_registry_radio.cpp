#include <NodeRegistryStore.h>
#include <RecoveryService.h>
#include <Preferences.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using Bytes = std::vector<uint8_t>;
std::map<std::string, std::map<std::string, Bytes>> flash;
std::string failingSpace;
bool failWrite = false, failRead = false, failReadback = false;
uint32_t clockMs = 0;
unsigned reads = 0, writes = 0;
uint32_t millis() { return clockMs; }
TickType_t xTaskGetTickCount() { return clockMs; }
int64_t esp_timer_get_time() { return static_cast<int64_t>(clockMs) * 1000; }
void esp_fill_random(void* output, size_t size) {
    static uint8_t next = 0;
    auto* bytes = static_cast<uint8_t*>(output);
    while (size--) *bytes++ = ++next;
}
bool Preferences::isKey(const char* key) { return flash[space_].count(key) != 0; }
size_t Preferences::getBytesLength(const char* key) { return flash[space_][key].size(); }
size_t Preferences::getBytes(const char* key, void* output, size_t capacity) {
    ++reads;
    if (space_ == failingSpace && failRead) return 0;
    const auto& bytes = flash[space_][key];
    if (capacity < bytes.size()) return 0;
    memcpy(output, bytes.data(), bytes.size());
    return bytes.size();
}
size_t Preferences::putBytes(const char* key, const void* data, size_t size) {
    ++writes;
    if (space_ == failingSpace && failWrite) return 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    flash[space_][key] = Bytes(bytes, bytes + size);
    if (space_ == failingSpace && failReadback) failRead = true;
    return size;
}
namespace gateway::recovery {
Guard::Guard(TickType_t) : held_(true) {}
Guard::~Guard() = default;
bool blocked() { return false; }
}
namespace n = radiosensors::registry;
namespace s = radiosensors::security;
namespace f = s::frames;
namespace r = radiosensors::replay;
namespace g = gateway::registry_store;

struct Fixture {
    s::Context node;
    uint8_t salt[s::kSaltSize]{};
    Fixture() {
        flash.clear(); failingSpace.clear();
        failWrite = failRead = failReadback = false;
        clockMs = 0; reads = writes = 0;
        assert(g::begin() && g::ready());
        uint8_t factory[16]{};
        osk::crypto::Cmac mac; osk::crypto::cmacInit(mac, factory);
        f::JoinRequest request;
        request.identity.uid[0] = 1; request.identity.requestNonce = 42;
        request.profileId = 6;
        uint8_t wire[f::kJoinRequestSize], accepted[f::kJoinAcceptSize];
        assert(f::sealJoinRequest(mac, {100,0,0}, request, wire, sizeof(wire)));
        assert(g::pairingRequestAndSave(mac, request.identity.uid, {100,0,0}, wire, sizeof(wire),
                                       123, {0,100,0}, accepted, sizeof(accepted)) == s::pairing::Status::Accept);
        f::JoinAccept accept;
        assert(f::openJoinAccept(mac, {0,100,0}, accepted, sizeof(accepted), accept));
        assert(accept.nodeId == 1);
        memcpy(salt, accept.salt, sizeof(salt)); s::initialize(node, factory, salt);
        uint8_t proof[f::kJoinConfirmSize], complete[f::kJoinCompleteSize];
        assert(f::sealJoinProof(node.authentication, {100,1,0}, request.identity, salt, false, proof, sizeof(proof)));
        assert(g::pairingConfirmAndSave(1, {100,1,0}, proof, sizeof(proof), complete, sizeof(complete)) == s::pairing::Status::Complete);
    }
    n::OpenedFrame receive(uint32_t counter, r::Action action, bool ready = false) {
        const uint8_t payload[] = {0, 33, 12, 0};
        uint8_t wire[s::kMaxWireSize]; size_t size = 0;
        assert(s::seal(node, s::Direction::Node, 1, {100,1,0x40}, counter,
                       ready ? f::kCommandReadyHeader : 0x60, ready ? nullptr : payload,
                       ready ? 0 : sizeof(payload), wire, sizeof(wire), size));
        n::OpenedFrame opened;
        assert(g::receiveSecure({100,1,0x40}, wire, size, true, true, opened) == n::ReceiveStatus::Ok);
        assert(opened.decision.action == action);
        return opened;
    }
};

void registryFault(bool readback) {
    Fixture x;
    x.receive(1024, r::Action::Accept, true);
    uint8_t reply[r::kMaxReplySize]; size_t size = 0;
    assert(g::commandReply(1, x.salt, 1024, f::kNoCommandHeader, nullptr, 0, reply, sizeof(reply), size));
    failingSpace = "node-reg";
    failWrite = !readback; failReadback = readback;
    n::RenameStatus renamed;
    assert(g::renameAndSave(1, "new", 3, renamed) == RegistryCommitStatus::StorageError);
    assert(!g::ready() && !g::isActiveNode(1));
    gateway::registry_store::Snapshot snapshot;
    assert(!g::snapshot(snapshot));
    size = 99;
    assert(!g::commandReply(1, x.salt, 1024, f::kNoCommandHeader, nullptr, 0, reply, sizeof(reply), size) && size == 0);
    const unsigned before = reads;
    const unsigned locksBefore = registryMutexTakes;
    clockMs = 999; g::loop(); assert(reads == before && !g::ready());
    assert(registryMutexTakes == locksBefore);
    if (readback) {
        clockMs = 1000; g::loop(); assert(!g::ready());
        const unsigned after = reads;
        clockMs = 1999; g::loop(); assert(reads == after);
    }
    failWrite = failRead = failReadback = false;
    clockMs = readback ? 2000 : 1000;
    g::loop(); assert(g::ready() && g::isActiveNode(1));
    assert(g::snapshot(snapshot) && snapshot.count == 1);
    assert(snapshot.records[0].displayNameLength == (readback ? 3 : 0));
    const auto stale = x.receive(1024, r::Action::CounterFloor, true);
    assert(stale.decision.floor == 1280 && stale.replySize == 0);
    x.receive(1280, r::Action::Accept);
}

void boundFault(bool readback) {
    Fixture x; x.receive(1024, r::Action::Accept);
    failingSpace = "radio-bound";
    failWrite = !readback; failReadback = readback;
    const uint8_t payload[] = {0, 33, 12, 0};
    uint8_t wire[s::kMaxWireSize]; size_t size = 0;
    assert(s::seal(x.node, s::Direction::Node, 1, {100,1,0x40}, 1280, 0x60, payload, sizeof(payload), wire, sizeof(wire), size));
    n::OpenedFrame opened;
    assert(g::receiveSecure({100,1,0x40}, wire, size, true, true, opened) == n::ReceiveStatus::StorageError);
    assert(!g::ready() && !g::isActiveNode(1));
    const unsigned before = reads;
    clockMs = 999; g::loop(); assert(reads == before);
    failWrite = failRead = failReadback = false;
    clockMs = 1000; g::loop(); assert(g::ready());
    const uint32_t floor = readback ? 1536 : 1280;
    assert(x.receive(1024, r::Action::CounterFloor).decision.floor == floor);
    if (readback) assert(x.receive(1280, r::Action::CounterFloor).decision.floor == floor);
    x.receive(floor, r::Action::Accept);
}

void historyReleasedAfterCommittedRemoval() {
    Fixture x; x.receive(1024,r::Action::Accept);
    assert(g::radioDiagnostics().historyBytes == sizeof(r::AcceptanceHistory));
    bool removed = false;
    assert(g::removeAndSave(1,removed) == RegistryCommitStatus::Ok && removed);
    assert(g::radioDiagnostics().historyBytes == 0);
    Fixture next; next.receive(1024,r::Action::Accept);
    size_t count = 0;
    assert(g::clearAndSave(count) == RegistryCommitStatus::Ok && count == 1);
    assert(g::radioDiagnostics().historyBytes == 0);
}
r::Snapshot durableBounds() {
    const auto& bytes = flash["radio-bound"]["bounds"];
    r::Snapshot snapshot; uint32_t generation;
    assert(r::decode(bytes.data(), bytes.size(), snapshot, generation));
    return snapshot;
}
void removalCleansBounds(bool clear, bool failRegistry, bool failCleanup, bool readback) {
    Fixture x; x.receive(1024, r::Action::Accept);
    assert(durableBounds().records[0].state == r::RecordState::Bound);
    if (failRegistry || failCleanup) {
        failingSpace = failRegistry ? "node-reg" : "radio-bound";
        failWrite = !readback; failReadback = readback;
    }
    bool removed = false; size_t count = 0;
    const auto status = clear ? g::clearAndSave(count) : g::removeAndSave(1, removed);
    assert(status == (failRegistry ? RegistryCommitStatus::StorageError : RegistryCommitStatus::Ok));
    assert(clear ? count == (failRegistry ? 0U : 1U) : removed == !failRegistry);
    if (failRegistry || failCleanup) {
        assert(!g::ready());
        if (!readback) assert(durableBounds().records[0].state == r::RecordState::Bound);
        failWrite = failRead = failReadback = false;
        clockMs = 1000; g::loop(); assert(g::ready());
    }
    g::Snapshot snapshot; assert(g::snapshot(snapshot));
    const bool retained = failRegistry && !readback;
    assert(snapshot.count == (retained ? 1U : 0U));
    assert(durableBounds().records[0].state == (retained ? r::RecordState::Bound : r::RecordState::Absent));
    if (retained) assert(x.receive(1024, r::Action::CounterFloor).decision.floor == 1280);
}
int main() {
    registryFault(false); registryFault(true);
    boundFault(false); boundFault(true);
    historyReleasedAfterCommittedRemoval();
    for (bool clear : {false, true}) {
        removalCleansBounds(clear, false, false, false);
        removalCleansBounds(clear, true, false, false);
        removalCleansBounds(clear, true, false, true);
        removalCleansBounds(clear, false, true, false);
        removalCleansBounds(clear, false, true, true);
    }
    std::cout << "Registry and replay faults: throttled reload, persistent read failure, durable floors and cache reset passed\n";
}
