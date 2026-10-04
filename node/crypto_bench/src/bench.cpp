#include <Arduino.h>
#include <CryptoSelfTest.h>
#include <OskCrypto.h>

using namespace osk::crypto;

namespace {

constexpr uint16_t kRuns = 64;
Cmac cmac;
uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
uint8_t block[16];
uint8_t tag[16];

uint32_t emptyLoop() {
    const uint32_t start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) __asm__ __volatile__("" ::: "memory");
    return micros() - start;
}

void report(const __FlashStringHelper* name, const uint32_t total) {
    const uint32_t overhead = emptyLoop();
    const uint32_t net = total > overhead ? total - overhead : 0;
    Serial.print(name);
    Serial.print(F(": "));
    Serial.print(net / kRuns);
    Serial.print(F(" us, "));
    Serial.print(net * (F_CPU / 1000000UL) / kRuns);
    Serial.println(F(" cycles"));
}

}  // namespace

void runBench() {
    Serial.print(F("F_CPU "));
    Serial.println(F_CPU);
    const uint8_t failed = selfTest();
    Serial.print(F("self-test mask 0x"));
    Serial.print(failed, HEX);
    Serial.println(failed ? F(" FAIL") : F(" PASS"));

    uint32_t start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) aesExpandKey(cmac.aes, key);
    report(F("key expansion"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) aesEncrypt(cmac.aes, block);
    report(F("AES block"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) cmacInit(cmac, key);
    report(F("CMAC init"), micros() - start);

    // One report: CTR over a 9-byte payload, then a tag over 16 bytes of
    // header and ciphertext.
    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        ctrApply(cmac.aes, key, block + 7, 9);
        cmacCompute(cmac, block, 16, tag);
    }
    report(F("seal uplink"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) cmacCompute(cmac, block, 8, tag);
    report(F("verify ACK"), micros() - start);
}

void setup() { Serial.begin(9600); }

// Repeats so a monitor opened after the upload still sees a full report.
void loop() {
    runBench();
    Serial.println();
    delay(3000);
}
