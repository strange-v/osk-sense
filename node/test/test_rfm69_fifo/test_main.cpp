// Drives the pinned RFM69 driver's FIFO parser through an emulated SX1231,
// so length bounds are checked in the code that copies radio bytes into
// RFM69::DATA, before any frame authentication. Built with AddressSanitizer:
// an out-of-bounds write into DATA fails the run even where an assertion
// would not notice it.
#include <RFM69.h>
#include <RFM69registers.h>
#include <deque>
#include <unity.h>

namespace {
constexpr uint8_t kNodeAddress = 1;
constexpr uint8_t kSender = 100;
constexpr size_t kFifoSize = 66;

uint8_t registers[0x80];
std::deque<uint8_t> fifo;
int spiAddress = -1;
bool spiWrite = false;
uint32_t now = 0;
void (*isr)() = nullptr;

uint8_t readRegister(uint8_t address) {
    switch (address) {
    case REG_IRQFLAGS1:
        return RF_IRQFLAGS1_MODEREADY;
    case REG_IRQFLAGS2:
        return fifo.empty() ? 0 : RF_IRQFLAGS2_PAYLOADREADY;
    case REG_RSSICONFIG:
        return RF_RSSI_DONE;
    default:
        return registers[address];
    }
}

void writeRegister(uint8_t address, uint8_t value) {
    if (address == REG_PACKETCONFIG2 && (value & RF_PACKET2_RXRESTART))
        fifo.clear();
    registers[address] = value;
}

// Loads a received frame as the packet handler leaves it: the length byte,
// then as many of the announced bytes as the FIFO holds.
void loadFrame(uint8_t length, uint8_t target = kNodeAddress) {
    fifo.clear();
    fifo.push_back(length);
    for (size_t i = 0; i < length && fifo.size() < kFifoSize; ++i)
        if (i == 0)
            fifo.push_back(target);
        else if (i == 1)
            fifo.push_back(kSender);
        else if (i == 2)
            fifo.push_back(0);
        else
            fifo.push_back(static_cast<uint8_t>(0xA0 + i));
}
} // namespace

SPIClass SPI;
void SPIClass::begin() {
}
void SPIClass::beginTransaction(SPISettings) {
    spiAddress = -1;
}
void SPIClass::endTransaction() {
}
uint8_t SPIClass::transfer(uint8_t value) {
    if (spiAddress < 0) {
        spiAddress = value & 0x7F;
        spiWrite = (value & 0x80) != 0;
        return 0;
    }
    if (spiAddress == REG_FIFO) {
        if (spiWrite || fifo.empty())
            return 0;
        const uint8_t byte = fifo.front();
        fifo.pop_front();
        return byte;
    }
    const uint8_t address = static_cast<uint8_t>(spiAddress++ & 0x7F);
    if (spiWrite) {
        writeRegister(address, value);
        return 0;
    }
    return readRegister(address);
}

uint32_t millis() {
    return now++;
}
uint32_t micros() {
    return now++ * 1000;
}
void delay(uint32_t) {
}
void delayMicroseconds(uint32_t) {
}
void digitalWrite(uint8_t, uint8_t) {
}
void pinMode(uint8_t, uint8_t) {
}
void attachInterrupt(uint8_t, void (*handler)(), int) {
    isr = handler;
}
void detachInterrupt(uint8_t) {
}
void noInterrupts() {
}
void interrupts() {
}

#if !defined(RF69_LISTENMODE_ENABLE)
namespace {
RFM69 radio(SS, 2, false, &SPI);

bool receive(uint8_t length) {
    radio.receiveDone(); // enter RX before the frame arrives
    loadFrame(length);
    isr();
    return radio.receiveDone();
}
} // namespace

void setUp() {
    fifo.clear();
    TEST_ASSERT_TRUE(radio.initialize(RF69_868MHZ, kNodeAddress, 1));
}
void tearDown() {
}

void assertFullBuffer() {
    TEST_ASSERT_EQUAL_UINT8(RF69_MAX_DATA_LEN + 3, RFM69::PAYLOADLEN);
    TEST_ASSERT_EQUAL_UINT8(RF69_MAX_DATA_LEN, RFM69::DATALEN);
    TEST_ASSERT_EQUAL_UINT16(kSender, RFM69::SENDERID);
    for (uint8_t i = 0; i < RF69_MAX_DATA_LEN; ++i)
        TEST_ASSERT_EQUAL_HEX8(0xA3 + i, RFM69::DATA[i]);
    TEST_ASSERT_EQUAL_HEX8(0, RFM69::DATA[RF69_MAX_DATA_LEN]);
}

void test_longest_frame_fills_buffer_and_terminates() {
    TEST_ASSERT_TRUE(receive(RF69_MAX_DATA_LEN + 3));
    assertFullBuffer();
}

void test_header_only_frame_is_empty() {
    TEST_ASSERT_TRUE(receive(3));
    TEST_ASSERT_EQUAL_UINT8(0, RFM69::DATALEN);
}

void test_frames_shorter_than_header_are_rejected() {
    for (uint8_t length = 0; length < 3; ++length) {
        TEST_ASSERT_FALSE(receive(length));
        TEST_ASSERT_EQUAL_UINT8(0, RFM69::DATALEN);
    }
}

// 65 and 66 fit the 66-byte FIFO and the default REG_PAYLOADLENGTH; larger
// length bytes cover a radio that delivers them anyway.
void test_frames_longer_than_buffer_are_truncated() {
    const uint8_t lengths[] = {RF69_MAX_DATA_LEN + 4, RF69_MAX_DATA_LEN + 5, 67, 128, 255};
    for (uint8_t length : lengths) {
        TEST_ASSERT_TRUE(receive(length));
        assertFullBuffer();
    }
}

void test_truncated_frame_does_not_block_next_one() {
    TEST_ASSERT_TRUE(receive(66));
    TEST_ASSERT_TRUE(receive(10));
    TEST_ASSERT_EQUAL_UINT8(7, RFM69::DATALEN);
    TEST_ASSERT_EQUAL_HEX8(0xA3, RFM69::DATA[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_longest_frame_fills_buffer_and_terminates);
    RUN_TEST(test_header_only_frame_is_empty);
    RUN_TEST(test_frames_shorter_than_header_are_rejected);
    RUN_TEST(test_frames_longer_than_buffer_are_truncated);
    RUN_TEST(test_truncated_frame_does_not_block_next_one);
    return UNITY_END();
}

#else
namespace {
// Listen-mode frames carry two burst-time bytes in place of the CTL byte.
class ListenRadio : public RFM69 {
  public:
    using RFM69::RFM69;
    void deliver(uint8_t length) {
        listenModeReset();
        loadFrame(length);
        listenModeInterruptHandler();
        fifo.clear();
    }
};
ListenRadio radio(SS, 2, false, &SPI);
} // namespace

void setUp() {
    TEST_ASSERT_TRUE(radio.initialize(RF69_868MHZ, kNodeAddress, 1));
}
void tearDown() {
}

void test_listen_frame_bounds() {
    radio.deliver(4);
    TEST_ASSERT_EQUAL_UINT8(0, RFM69::DATALEN);
    TEST_ASSERT_EQUAL_UINT8(4, RFM69::PAYLOADLEN);
    radio.deliver(64);
    TEST_ASSERT_EQUAL_UINT8(60, RFM69::DATALEN);
}

void test_listen_frames_shorter_than_header_are_rejected() {
    for (uint8_t length = 0; length < 4; ++length) {
        radio.deliver(length);
        TEST_ASSERT_EQUAL_UINT8(0, RFM69::DATALEN);
        TEST_ASSERT_EQUAL_UINT8(0, RFM69::PAYLOADLEN);
    }
}

void test_listen_frames_longer_than_buffer_are_truncated() {
    const uint8_t lengths[] = {65, 66, 255};
    for (uint8_t length : lengths) {
        radio.deliver(length);
        TEST_ASSERT_EQUAL_UINT8(64, RFM69::PAYLOADLEN);
        TEST_ASSERT_EQUAL_UINT8(60, RFM69::DATALEN);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_listen_frame_bounds);
    RUN_TEST(test_listen_frames_shorter_than_header_are_rejected);
    RUN_TEST(test_listen_frames_longer_than_buffer_are_truncated);
    return UNITY_END();
}
#endif
