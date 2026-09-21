#include <gtest/gtest.h>
#include "../../variants/heltec_v4/SolarCompanion.h"

TEST(SolarCompanion, ThreeConsecutiveLowSamplesRequired) {
  uint8_t count = 0;
  EXPECT_FALSE(SolarCompanion::sustainedLow(3350, count));
  EXPECT_FALSE(SolarCompanion::sustainedLow(3300, count));
  EXPECT_TRUE(SolarCompanion::sustainedLow(3399, count));
}

TEST(SolarCompanion, TransientSagAndMissingBatteryDoNotAccumulate) {
  uint8_t count = 0;
  EXPECT_FALSE(SolarCompanion::sustainedLow(3350, count));
  EXPECT_FALSE(SolarCompanion::sustainedLow(3400, count));
  EXPECT_EQ(count, 0);
  EXPECT_FALSE(SolarCompanion::sustainedLow(3350, count));
  EXPECT_FALSE(SolarCompanion::sustainedLow(0, count));
  EXPECT_EQ(count, 0);
}

TEST(SolarCompanion, RecoveryNeedsTwoHealthyReadingsAboveHysteresis) {
  uint8_t count = 0;
  EXPECT_FALSE(SolarCompanion::recharged(3500, count));
  EXPECT_FALSE(SolarCompanion::recharged(3699, count));
  EXPECT_FALSE(SolarCompanion::recharged(3700, count));
  EXPECT_TRUE(SolarCompanion::recharged(3750, count));
}

TEST(SolarCompanion, CloudSagResetsRecoveryQualification) {
  uint8_t count = 0;
  EXPECT_FALSE(SolarCompanion::recharged(3800, count));
  EXPECT_FALSE(SolarCompanion::recharged(3600, count));
  EXPECT_FALSE(SolarCompanion::recharged(3800, count));
  EXPECT_TRUE(SolarCompanion::recharged(3800, count));
}

TEST(SolarCompanion, InvalidAdcCannotProveRecovery) {
  uint8_t count = 1;
  EXPECT_FALSE(SolarCompanion::recharged(0, count));
  EXPECT_EQ(count, 0);
  count = 1;
  EXPECT_FALSE(SolarCompanion::recharged(6000, count));
  EXPECT_EQ(count, 0);
}

TEST(SolarCompanion, CountersSaturateAndWakeRemainsFinite) {
  uint8_t low = 0, high = 0;
  for (int i = 0; i < 1000; ++i) {
    SolarCompanion::sustainedLow(3300, low);
    SolarCompanion::recharged(4000, high);
  }
  EXPECT_EQ(low, 3);
  EXPECT_EQ(high, 2);
  EXPECT_GT(SolarCompanion::recoverySeconds, 0);
  EXPECT_LE(SolarCompanion::recoverySeconds, 60);
  EXPECT_GT(SolarCompanion::resumeMv, SolarCompanion::cutoffMv);
}
