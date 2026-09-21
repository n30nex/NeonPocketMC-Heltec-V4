#pragma once

#include <stdint.h>

namespace SolarCompanion {
constexpr uint16_t cutoffMv = 3400;
constexpr uint16_t resumeMv = 3700;
constexpr uint32_t recoverySeconds = 60;
constexpr uint32_t sampleIntervalMs = 8000;

inline bool lowVoltage(uint16_t mv) { return mv > 0 && mv < cutoffMv; }

inline bool sustainedLow(uint16_t mv, uint8_t& samples) {
  samples = lowVoltage(mv) ? (samples < 3 ? samples + 1 : 3) : 0;
  return samples >= 3;
}

inline bool recharged(uint16_t mv, uint8_t& samples) {
  // Reject disconnected/out-of-range ADC readings as proof of recovery.
  samples = mv >= resumeMv && mv <= 4500 ? (samples < 2 ? samples + 1 : 2) : 0;
  return samples >= 2;
}

inline bool bootNeedsRecovery(uint16_t mv, bool recovering, bool faultReset, uint8_t& samples) {
  if (recovering || faultReset) {
    if (faultReset) samples = 0;
    return !recharged(mv, samples);
  }
  samples = 0;
  return lowVoltage(mv);
}

void checkBoot();
void beginPowerSaving();
void loop();
[[noreturn]] void sleepForRecharge(bool radioReady);
}
