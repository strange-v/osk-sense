#include <RadioAes.h>
#include <unity.h>

#include <string.h>

using namespace radiosensors::radio_aes;

namespace {

class FakeRadio {
  public:
    FakeRadio() {
        memset(registers, 0, sizeof(registers));
    }

    void setMode(const uint8_t value) {
        mode = value;
    }
    uint8_t readReg(const uint8_t address) {
        return registers[address];
    }
    void writeReg(const uint8_t address, const uint8_t value) {
        // AES registers only take writes in standby.
        if (mode != kStandbyMode)
            return;
        registers[address] = value;
    }

    uint8_t mode = 0;
    uint8_t registers[0x80];
};

} // namespace

void setUp() {
}
void tearDown() {
}

void test_key_with_a_leading_zero_turns_aes_on() {
    FakeRadio radio;
    const uint8_t key[kKeySize] = {0x00,
                                   0x11,
                                   0x22,
                                   0x00,
                                   0x44,
                                   0x55,
                                   0x66,
                                   0x77,
                                   0x88,
                                   0x99,
                                   0xAA,
                                   0xBB,
                                   0xCC,
                                   0xDD,
                                   0xEE,
                                   0xFF};
    enable(radio, key);
    TEST_ASSERT_EQUAL_UINT8(kStandbyMode, radio.mode);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key, &radio.registers[kAesKey1Register], kKeySize);
    TEST_ASSERT_EQUAL_HEX8(kAesOn, radio.registers[kPacketConfig2Register] & kAesOn);
}

void test_other_packet_config_bits_are_kept() {
    FakeRadio radio;
    radio.registers[kPacketConfig2Register] = 0x12;
    const uint8_t key[kKeySize] = {};
    enable(radio, key);
    TEST_ASSERT_EQUAL_HEX8(0x13, radio.registers[kPacketConfig2Register]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_key_with_a_leading_zero_turns_aes_on);
    RUN_TEST(test_other_packet_config_bits_are_kept);
    return UNITY_END();
}
