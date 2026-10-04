#pragma once

#include <RadioSecurityStorage.h>
#include <unity.h>

namespace security_store_tests {
namespace secure = radiosensors::node::security_storage;
namespace storage = radiosensors::node::storage;
struct PowerLoss {};
struct Eeprom {
    Eeprom() { memset(bytes, 0xFF, sizeof(bytes)); }
    uint8_t read(size_t address) const { return bytes[address]; }
    void update(size_t address, uint8_t value) {
        if (remaining == 0) { if (throwOnFailure) throw PowerLoss{}; return; }
        if (remaining > 0) --remaining;
        bytes[address] = value;
        ++updates;
    }
    uint8_t bytes[256];
    int remaining = -1;
    bool throwOnFailure = false;
    unsigned updates = 0;
};
secure::NetworkConfig config() {
    secure::NetworkConfig value;
    value.state = storage::ProvisioningState::Active;
    value.nodeId = 7; value.gatewayId = 100; value.networkId = 42;
    for (uint8_t i = 0; i < 8; ++i) value.salt[i] = 0x10 + i;
    value.requestNonce = UINT64_C(0x0123456789abcdef);
    return value;
}
void seed(Eeprom& memory, size_t address, uint32_t limit) {
    secure::encodeReserve(limit, memory.bytes + address);
}
void test_config_layout_and_crc() {
    auto value = config();
    uint8_t bytes[26];
    TEST_ASSERT_TRUE(secure::encodeConfig(value, bytes, sizeof(bytes)));
    const uint8_t expected[] = {'R','N',3,0,2,7,100,42,16,17,18,19,20,21,22,23,
        0xef,0xcd,0xab,0x89,0x67,0x45,0x23,1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, bytes, sizeof(expected));
    secure::NetworkConfig decoded;
    TEST_ASSERT_TRUE(secure::decodeConfig(bytes, sizeof(bytes), decoded));
    TEST_ASSERT_EQUAL_UINT64(value.requestNonce, decoded.requestNonce);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] ^= 1;
        TEST_ASSERT_FALSE(secure::decodeConfig(bytes, sizeof(bytes), decoded));
        bytes[i] ^= 1;
    }
    TEST_ASSERT_FALSE(secure::decodeConfig(bytes, 25, decoded));
}
void test_config_torn_write_is_atomic() {
    Eeprom initial;
    secure::NetworkConfigStore<Eeprom> first(initial);
    auto previous = config();
    TEST_ASSERT_TRUE(first.save(previous));
    for (int cut = 0; cut <= 28; ++cut) {
        Eeprom memory = initial;
        secure::NetworkConfigStore<Eeprom> writer(memory);
        secure::NetworkConfig value;
        TEST_ASSERT_TRUE(writer.load(value));
        value.salt[0] ^= 0x80;
        ++value.requestNonce;
        memory.remaining = cut; memory.throwOnFailure = true;
        try { writer.save(value); } catch (const PowerLoss&) {}
        memory.remaining = -1;
        secure::NetworkConfigStore<Eeprom> reboot(memory);
        TEST_ASSERT_TRUE(reboot.load(value));
        TEST_ASSERT_TRUE(value.generation == 0 || value.generation == 1);
        TEST_ASSERT_EQUAL_UINT64(previous.requestNonce + value.generation, value.requestNonce);
        TEST_ASSERT_EQUAL_HEX8(previous.salt[0] ^ (value.generation ? 0x80 : 0), value.salt[0]);
    }
}
void test_config_wrap_reset_and_readback() {
    Eeprom memory;
    seed(memory, secure::kReserveA, 1024); seed(memory, secure::kReserveB, 2048);
    memset(memory.bytes + 64, 0xA5, 192);
    uint8_t original[256]; memcpy(original, memory.bytes, sizeof(original));
    secure::NetworkConfigStore<Eeprom> store(memory);
    auto value = config();
    for (unsigned i = 0; i < 257; ++i) { value.requestNonce = i; TEST_ASSERT_TRUE(store.save(value)); }
    TEST_ASSERT_TRUE(store.load(value));
    TEST_ASSERT_EQUAL(0, value.generation);
    TEST_ASSERT_EQUAL_UINT64(256, value.requestNonce);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original + 26, memory.bytes + 26, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original + 58, memory.bytes + 58, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original + 64, memory.bytes + 64, 192);
    memory.remaining = 10;
    TEST_ASSERT_FALSE(store.save(value));
    TEST_ASSERT_EQUAL(0, value.generation);
    memory.remaining = 0;
    TEST_ASSERT_FALSE(store.factoryReset());
    memory.remaining = -1;
    uint8_t before[256]; memcpy(before, memory.bytes, sizeof(before));
    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_FALSE(store.load(value));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before + 26, memory.bytes + 26, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before + 58, memory.bytes + 58, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before + 64, memory.bytes + 64, 192);
    secure::FrameCounterStore<Eeprom> counter(memory);
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::Loaded, counter.load(false));
    uint32_t issued;
    TEST_ASSERT_TRUE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(2048, issued);
}
void test_counter_lazy_reservation_and_boot_skip() {
    Eeprom memory;
    secure::FrameCounterStore<Eeprom> counter(memory);
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::Fresh, counter.load(false));
    TEST_ASSERT_EQUAL(0, memory.updates);
    uint32_t issued;
    for (unsigned i = 0; i < 1024; ++i) {
        TEST_ASSERT_TRUE(counter.take(issued));
        TEST_ASSERT_EQUAL_UINT32(i, issued);
    }
    TEST_ASSERT_EQUAL(6, memory.updates);
    TEST_ASSERT_TRUE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(1024, issued);
    TEST_ASSERT_EQUAL(12, memory.updates);
    secure::FrameCounterStore<Eeprom> reboot(memory);
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::Loaded, reboot.load(true));
    TEST_ASSERT_EQUAL(12, memory.updates);
    TEST_ASSERT_TRUE(reboot.take(issued));
    TEST_ASSERT_EQUAL_UINT32(2048, issued);
    TEST_ASSERT_EQUAL(18, memory.updates);
}
void test_counter_corruption_requires_discarding_keys() {
    Eeprom memory;
    secure::FrameCounterStore<Eeprom> counter(memory);
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::KeysMustBeDiscarded, counter.load(true));
    uint32_t issued = 99;
    TEST_ASSERT_FALSE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(99, issued);
    seed(memory, secure::kReserveA, 2048); seed(memory, secure::kReserveB, 1024);
    counter.load(true);
    TEST_ASSERT_EQUAL_UINT32(2048, counter.limit());
    memory.bytes[30] ^= 1;
    counter.load(true);
    TEST_ASSERT_EQUAL_UINT32(1024, counter.limit());
    memory.bytes[62] ^= 1;
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::KeysMustBeDiscarded, counter.load(true));
    TEST_ASSERT_FALSE(counter.take(issued));
    TEST_ASSERT_EQUAL(secure::CounterLoadStatus::Fresh, counter.load(false));
    TEST_ASSERT_TRUE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(0, issued);
}
void test_counter_torn_write_never_reuses_counters() {
    Eeprom initial;
    secure::FrameCounterStore<Eeprom> first(initial);
    first.load(false);
    uint32_t issued;
    for (unsigned i = 0; i < 1024; ++i) TEST_ASSERT_TRUE(first.take(issued));
    for (int cut = 0; cut <= 6; ++cut) {
        Eeprom memory = initial;
        secure::FrameCounterStore<Eeprom> writer(memory);
        writer.load(true);
        memory.remaining = cut; memory.throwOnFailure = true;
        uint32_t attempted = issued;
        bool success = false;
        try { success = writer.take(attempted); } catch (const PowerLoss&) {}
        memory.remaining = -1;
        secure::FrameCounterStore<Eeprom> reboot(memory);
        TEST_ASSERT_EQUAL(secure::CounterLoadStatus::Loaded, reboot.load(true));
        uint32_t after;
        TEST_ASSERT_TRUE(reboot.take(after));
        TEST_ASSERT_TRUE(after > (success ? attempted : issued));
    }
}
void test_counter_readback_failure_stops_transmission() {
    Eeprom memory;
    secure::FrameCounterStore<Eeprom> counter(memory);
    counter.load(false); memory.remaining = 5;
    uint32_t issued = 99;
    TEST_ASSERT_FALSE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(99, issued);
    memory.remaining = -1;
    TEST_ASSERT_FALSE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(0, counter.limit());
}
void test_counter_floor_bounded_and_one_reservation() {
    Eeprom memory;
    secure::FrameCounterStore<Eeprom> counter(memory);
    counter.load(false);
    uint32_t issued;
    TEST_ASSERT_TRUE(counter.take(issued));
    TEST_ASSERT_FALSE(counter.advanceToFloor(issued, 0));
    TEST_ASSERT_FALSE(counter.advanceToFloor(issued, 257));
    TEST_ASSERT_FALSE(counter.advanceToFloor(issued + 1, 256));
    TEST_ASSERT_EQUAL(6, memory.updates);
    TEST_ASSERT_TRUE(counter.advanceToFloor(issued, 256));
    TEST_ASSERT_EQUAL(12, memory.updates);
    TEST_ASSERT_EQUAL_UINT32(1280, counter.limit());
    TEST_ASSERT_FALSE(counter.advanceToFloor(issued, 256));
    TEST_ASSERT_TRUE(counter.take(issued));
    TEST_ASSERT_EQUAL_UINT32(256, issued);
    TEST_ASSERT_EQUAL(12, memory.updates);
    secure::FrameCounterStore<Eeprom> reboot(memory);
    reboot.load(true); TEST_ASSERT_TRUE(reboot.take(issued));
    TEST_ASSERT_EQUAL_UINT32(1280, issued);
}
void test_counter_floor_torn_write_and_overflow() {
    Eeprom initial;
    secure::FrameCounterStore<Eeprom> first(initial);
    first.load(false);
    uint32_t issued;
    TEST_ASSERT_TRUE(first.take(issued));
    for (int cut = 0; cut < 6; ++cut) {
        Eeprom memory = initial;
        secure::FrameCounterStore<Eeprom> writer(memory);
        writer.load(true);
        uint32_t current;
        TEST_ASSERT_TRUE(writer.take(current));
        memory.remaining = cut;
        TEST_ASSERT_FALSE(writer.advanceToFloor(current, current + 256));
        TEST_ASSERT_FALSE(writer.take(issued));
        memory.remaining = -1;
        secure::FrameCounterStore<Eeprom> reboot(memory);
        reboot.load(true); TEST_ASSERT_TRUE(reboot.take(issued));
        TEST_ASSERT_TRUE(issued > current);
    }
    Eeprom exhausted;
    seed(exhausted, secure::kReserveA, UINT32_MAX - 512);
    secure::FrameCounterStore<Eeprom> counter(exhausted);
    counter.load(true);
    TEST_ASSERT_FALSE(counter.take(issued));
    counter.load(false);
    TEST_ASSERT_FALSE(counter.take(issued));
    TEST_ASSERT_EQUAL(0, exhausted.updates);
    Eeprom boundary;
    seed(boundary, secure::kReserveA, UINT32_MAX - 1024);
    secure::FrameCounterStore<Eeprom> lastRange(boundary);
    lastRange.load(true);
    for (uint32_t i = 0; i < 1024; ++i) {
        TEST_ASSERT_TRUE(lastRange.take(issued));
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1024 + i, issued);
    }
    TEST_ASSERT_FALSE(lastRange.take(issued));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, issued);
    TEST_ASSERT_EQUAL(6, boundary.updates);
    lastRange.load(false);
    TEST_ASSERT_FALSE(lastRange.take(issued));
}
}
