#include <PairingEntropy.h>
#include <unity.h>
#include <fstream>
#include <vector>
#include <string.h>

namespace e = radiosensors::node::pairing_entropy;
namespace {
struct Source {
    std::vector<uint16_t> values;
    size_t position = 0;
    bool next(uint16_t& out) {
        if (position == values.size()) return false;
        out = values[position++]; return true;
    }
};
Source balanced() {
    Source source;
    // Each group extracts 0,1,0,1. Different upper capture bits are ignored.
    for (unsigned i = 0; i < e::kSampleCount / 4; ++i) {
        source.values.push_back(7766); source.values.push_back(7767);
        source.values.push_back(7767); source.values.push_back(7766);
    }
    return source;
}
void unchanged(const uint8_t (&out)[8]) { for (uint8_t b : out) TEST_ASSERT_EQUAL_HEX8(0xA5,b); }
}
void setUp() {}
void tearDown() {}
void test_balanced_pairs_extract_without_upper_capture_bits() {
    auto source = balanced(); uint8_t out[8]; memset(out,0xA5,8);
    TEST_ASSERT_TRUE(e::collect(source,out));
    for (uint8_t b : out) TEST_ASSERT_EQUAL_HEX8(0xAA,b);
    TEST_ASSERT_EQUAL_UINT(e::kSampleCount,source.position);
}
void test_stuck_biased_and_correlated_sequences_fail_without_output() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        Source source;
        for (unsigned i = 0; i < e::kSampleCount; ++i) {
            const uint8_t bit = mode == 0 ? 0 : mode == 1 ? 1 : mode == 2 ? i % 2 :
                mode == 3 ? (i % 8 == 0) : (i % 8 != 0);
            source.values.push_back(7766 + bit);
        }
        uint8_t out[8]; memset(out,0xA5,8);
        TEST_ASSERT_FALSE(e::collect(source,out)); unchanged(out);
        if (mode >= 3) TEST_ASSERT_EQUAL_UINT(e::kWindow,source.position);
    }
}
void test_biased_extracted_output_fails_despite_balanced_raw_input() {
    for (unsigned invert = 0; invert < 2; ++invert) {
        Source source;
        for (unsigned pair = 0; pair < e::kSampleCount / 2; ++pair) {
            const uint8_t bit = (pair % 4 == 0 && pair != 0) ^ invert;
            source.values.push_back(7766 + bit); source.values.push_back(7766 + (bit ^ 1));
        }
        uint8_t out[8]; memset(out,0xA5,8);
        TEST_ASSERT_FALSE(e::collect(source,out)); unchanged(out);
        TEST_ASSERT_EQUAL_UINT(e::kSampleCount,source.position);
    }
}
void test_insufficient_pairs_and_capture_timeout_fail_without_output() {
    auto source = balanced(); source.values.resize(511);
    uint8_t out[8]; memset(out,0xA5,8);
    TEST_ASSERT_FALSE(e::collect(source,out)); unchanged(out);
    source.values.clear(); source.position = 0;
    // Raw input passes balance/run checks but each equal pair is discarded.
    for (unsigned i = 0; i < e::kSampleCount; ++i) source.values.push_back(7766 + ((i / 2) % 2));
    TEST_ASSERT_FALSE(e::collect(source,out)); unchanged(out);
}
void test_bad_tail_cannot_hide_behind_early_extracted_bits() {
    auto source = balanced();
    for (unsigned i = 256; i < e::kSampleCount; ++i) source.values[i] = 7766;
    uint8_t out[8]; memset(out,0xA5,8);
    TEST_ASSERT_FALSE(e::collect(source,out)); unchanged(out);
}
void test_measured_capture_all_bursts_pass_health_checks() {
    std::ifstream file("crypto_bench/entropy-capture-samples.txt"); TEST_ASSERT_TRUE(file.good());
    unsigned bursts = 0;
    while (true) {
        Source source; uint16_t capture;
        for (unsigned i = 0; i < e::kSampleCount && file >> capture; ++i) source.values.push_back(capture);
        if (source.values.empty()) break;
        TEST_ASSERT_EQUAL_UINT(e::kSampleCount,source.values.size());
        uint8_t out[8]; TEST_ASSERT_TRUE(e::collect(source,out)); ++bursts;
    }
    TEST_ASSERT_EQUAL_UINT(8,bursts);
}
void test_cmac_conditioning_binds_uid_counter_and_entropy() {
    const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t uid[10] = {0,1,2,3,4,5,6,7,8,9}; uint8_t material[8] = {16,17,18,19,20,21,22,23};
    osk::crypto::Cmac mac; osk::crypto::cmacInit(mac,factory);
    const uint32_t value = e::condition(mac,uid,0x12345678,material);
    TEST_ASSERT_EQUAL_HEX32(0xCB9AB422,value);
    TEST_ASSERT_NOT_EQUAL(value,e::condition(mac,uid,0x12345679,material));
    ++uid[0]; TEST_ASSERT_NOT_EQUAL(value,e::condition(mac,uid,0x12345678,material)); --uid[0];
    ++material[0]; TEST_ASSERT_NOT_EQUAL(value,e::condition(mac,uid,0x12345678,material));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_balanced_pairs_extract_without_upper_capture_bits);
    RUN_TEST(test_stuck_biased_and_correlated_sequences_fail_without_output);
    RUN_TEST(test_biased_extracted_output_fails_despite_balanced_raw_input);
    RUN_TEST(test_insufficient_pairs_and_capture_timeout_fail_without_output);
    RUN_TEST(test_bad_tail_cannot_hide_behind_early_extracted_bits);
    RUN_TEST(test_measured_capture_all_bursts_pass_health_checks);
    RUN_TEST(test_cmac_conditioning_binds_uid_counter_and_entropy);
    return UNITY_END();
}
