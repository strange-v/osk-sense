#include <RadioPowerControl.h>
#include <unity.h>

using namespace radiosensors::radio_power;

namespace {

uint8_t report(ControlState &state,
               uint8_t level,
               int16_t rssi,
               uint8_t policy = kPolicyAuto,
               uint8_t ceiling = 31,
               bool fallback = false) {
    return observe(state, policy, ceiling, Report{level, fallback, rssi});
}

} // namespace

void setUp() {
}
void tearDown() {
}

void test_policy_encoding() {
    TEST_ASSERT_FALSE(isFixedPolicy(kPolicyAuto));
    TEST_ASSERT_TRUE(isFixedPolicy(fixedPolicy(0)));
    TEST_ASSERT_EQUAL_UINT8(0, fixedLevel(fixedPolicy(0)));
    TEST_ASSERT_EQUAL_UINT8(31, fixedLevel(fixedPolicy(31)));
    TEST_ASSERT_TRUE(validPolicy(fixedPolicy(31)));
    TEST_ASSERT_FALSE(validPolicy(33));
}

void test_first_report_keeps_the_reported_level() {
    ControlState state{};
    TEST_ASSERT_EQUAL_UINT8(10, report(state, 10, -40));
}

void test_strong_link_steps_down_after_enough_reports() {
    ControlState state{};
    TEST_ASSERT_EQUAL_UINT8(10, report(state, 10, -60));
    TEST_ASSERT_EQUAL_UINT8(10, report(state, 10, -60));
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 10, -60));

    // A new level starts a new average.
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 7, -63));
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 7, -63));
    TEST_ASSERT_EQUAL_UINT8(4, report(state, 7, -63));
}

void test_link_inside_the_window_is_left_alone() {
    ControlState state{};
    for (int index = 0; index < 5; ++index)
        TEST_ASSERT_EQUAL_UINT8(9, report(state, 9, -80));
    TEST_ASSERT_EQUAL_UINT8(9, report(state, 9, -76));
}

void test_weak_link_steps_up_within_the_ceiling() {
    ControlState state{};
    report(state, 5, -95, kPolicyAuto, 8);
    report(state, 5, -95, kPolicyAuto, 8);
    TEST_ASSERT_EQUAL_UINT8(8, report(state, 5, -95, kPolicyAuto, 8));

    ControlState roomy{};
    report(roomy, 5, -84);
    report(roomy, 5, -90);
    TEST_ASSERT_EQUAL_UINT8(11, report(roomy, 5, -90));
}

void test_fixed_policy_is_clamped_to_the_ceiling() {
    ControlState state{};
    TEST_ASSERT_EQUAL_UINT8(4, report(state, 2, -70, fixedPolicy(4), 31));
    ControlState limited{};
    TEST_ASSERT_EQUAL_UINT8(2, report(limited, 2, -70, fixedPolicy(4), 2));
}

void test_fixed_level_is_requested_again_after_a_fallback() {
    ControlState state{};
    TEST_ASSERT_EQUAL_UINT8(1, report(state, 6, -70, fixedPolicy(1), 6));
    report(state, 1, -95, fixedPolicy(1), 6);
    TEST_ASSERT_EQUAL_UINT8(1, report(state, 6, -80, fixedPolicy(1), 6, true));
    TEST_ASSERT_EQUAL_UINT8(1, report(state, 6, -80, fixedPolicy(1), 6, true));
}

void test_fallback_keeps_automatic_control_above_the_failed_level() {
    ControlState state{};
    report(state, 3, -84);
    // The node lost the gateway at level 3 and fell back to its ceiling.
    TEST_ASSERT_EQUAL_UINT8(20, report(state, 20, -55, kPolicyAuto, 20, true));
    TEST_ASSERT_EQUAL_UINT8(6, state.floor);
    report(state, 20, -55, kPolicyAuto, 20, true);
    TEST_ASSERT_EQUAL_UINT8(17, report(state, 20, -55, kPolicyAuto, 20, true));

    for (uint8_t level = 17; level > 6; level = state.desired) {
        report(state, level, -55, kPolicyAuto, 20);
        report(state, level, -55, kPolicyAuto, 20);
        report(state, level, -55, kPolicyAuto, 20);
    }
    TEST_ASSERT_EQUAL_UINT8(6, state.desired);
    report(state, 6, -55, kPolicyAuto, 20);
    report(state, 6, -55, kPolicyAuto, 20);
    TEST_ASSERT_EQUAL_UINT8(6, report(state, 6, -55, kPolicyAuto, 20));
}

void test_policy_change_applies_before_the_next_report() {
    ControlState state{};
    TEST_ASSERT_EQUAL_UINT8(kNoLevel, changePolicy(state, kPolicyAuto, 31));
    TEST_ASSERT_EQUAL_UINT8(9, changePolicy(state, fixedPolicy(9), 31));
    TEST_ASSERT_EQUAL_UINT8(2, changePolicy(state, fixedPolicy(9), 2));

    report(state, 12, -60);
    report(state, 12, -60);
    TEST_ASSERT_EQUAL_UINT8(12, changePolicy(state, kPolicyAuto, 31));
    // The average restarts, so three reports are needed again.
    TEST_ASSERT_EQUAL_UINT8(12, report(state, 12, -60));
    TEST_ASSERT_EQUAL_UINT8(12, report(state, 12, -60));
    TEST_ASSERT_EQUAL_UINT8(9, report(state, 12, -60));
}

void test_restarted_node_is_sent_back_to_the_wanted_level() {
    ControlState state{};
    report(state, 10, -60, kPolicyAuto, 20);
    report(state, 10, -60, kPolicyAuto, 20);
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 10, -60, kPolicyAuto, 20));
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 7, -80, kPolicyAuto, 20));

    // The node restarted at its ceiling, without a fallback.
    TEST_ASSERT_EQUAL_UINT8(7, report(state, 20, -70, kPolicyAuto, 20));
    for (int index = 0; index < 4; ++index)
        TEST_ASSERT_EQUAL_UINT8(7, report(state, 7, -80, kPolicyAuto, 20));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_policy_encoding);
    RUN_TEST(test_first_report_keeps_the_reported_level);
    RUN_TEST(test_strong_link_steps_down_after_enough_reports);
    RUN_TEST(test_link_inside_the_window_is_left_alone);
    RUN_TEST(test_weak_link_steps_up_within_the_ceiling);
    RUN_TEST(test_fixed_policy_is_clamped_to_the_ceiling);
    RUN_TEST(test_fixed_level_is_requested_again_after_a_fallback);
    RUN_TEST(test_fallback_keeps_automatic_control_above_the_failed_level);
    RUN_TEST(test_policy_change_applies_before_the_next_report);
    RUN_TEST(test_restarted_node_is_sent_back_to_the_wanted_level);
    return UNITY_END();
}
