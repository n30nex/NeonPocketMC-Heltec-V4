#ifdef HELTEC_V4_SOLAR_COMPANION

#include "SolarCompanion.h"
#include "target.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

#if !CONFIG_PM_ENABLE || !CONFIG_BT_CTRL_MODEM_SLEEP
#error "Solar BLE requires the pinned power-management/BLE-sleep Arduino core"
#endif

namespace SolarCompanion {
static constexpr uint32_t recoveryMarker = 0x534F4C34;
RTC_DATA_ATTR static uint32_t recovery = 0;
RTC_DATA_ATTR static uint8_t chargedSamples = 0;
RTC_DATA_ATTR static float adcMultiplier = 0;
static uint8_t lowSamples = 0;
static uint32_t lastSample = 0;
static bool watchdogReady = false;

[[noreturn]] void sleepForRecharge(bool radioReady) {
  recovery = recoveryMarker;
  adcMultiplier = board.getAdcMultiplier();

  // Never enter indefinite sleep, including UI power-off and startup failures.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (esp_sleep_enable_timer_wakeup(uint64_t(recoverySeconds) * 1000000ULL) != ESP_OK) {
    Serial.println("SOLAR: wake timer failed; restarting instead of sleeping");
    delay(1000);
    esp_restart();
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
  if (radioReady) radio_driver.powerOff();
  board.loRaFEMControl.setSleepModeEnable();
  rtc_gpio_hold_en((gpio_num_t)P_LORA_GC1109_PA_EN);

  // The optional GNSS rail must not remain powered during recharge either.
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, !PIN_GPS_EN_ACTIVE);
  gpio_hold_en((gpio_num_t)PIN_GPS_EN);

  // Also safe before SPI/radio/display initialization on a low-voltage boot.
  pinMode(P_LORA_RESET, OUTPUT);
  digitalWrite(P_LORA_RESET, LOW);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_RESET);
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
  digitalWrite(P_LORA_PA_POWER, LOW);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_PA_POWER);
  pinMode(PIN_VEXT_EN, OUTPUT);
  digitalWrite(PIN_VEXT_EN, LOW);
  gpio_hold_en((gpio_num_t)PIN_VEXT_EN);
  gpio_deep_sleep_hold_en();
  Serial.printf("SOLAR: recovery sleep %lu s; resume >= %u mV twice\n",
                (unsigned long)recoverySeconds, resumeMv);
  Serial.flush();
  esp_deep_sleep_start();
  esp_restart(); // Defensive fallback if the sleep API ever returns.
}

void checkBoot() {
  if (esp_task_wdt_init(30, true) == ESP_OK) {
    watchdogReady = esp_task_wdt_status(nullptr) == ESP_OK || esp_task_wdt_add(nullptr) == ESP_OK;
  }
  Serial.printf("SOLAR: task watchdog=%s\n", watchdogReady ? "ready" : "unavailable");
  if (recovery == recoveryMarker && adcMultiplier >= 1.0f && adcMultiplier <= 10.0f) {
    board.setAdcMultiplier(adcMultiplier);
  }
  const uint16_t mv = board.getBattMilliVolts();
  const auto reset = esp_reset_reason();
  const bool faultReset = reset == ESP_RST_BROWNOUT || reset == ESP_RST_TASK_WDT
      || reset == ESP_RST_INT_WDT || reset == ESP_RST_WDT;
  Serial.printf("SOLAR: battery=%u mV reset=%lu recovery=%u\n", mv,
                (unsigned long)esp_reset_reason(), recovery == recoveryMarker || faultReset);
  if (bootNeedsRecovery(mv, recovery == recoveryMarker, faultReset, chargedSamples)) {
    sleepForRecharge(false);
  }
  recovery = 0;
  chargedSamples = 0;
}

void beginPowerSaving() {
  const esp_err_t ble = esp_bt_sleep_enable();
  // Keep GPIO edge interrupts and the BLE host available continuously. Forced
  // CPU light sleep needs separate IRQ/USB qualification on the physical board.
  const esp_pm_config_esp32s3_t config = {80, 40, false};
  const esp_err_t pm = esp_pm_configure(&config);
  Serial.printf("SOLAR: BLE modem sleep=%s; CPU 40-80 MHz=%s; LoRa continuous RX\n",
                esp_err_to_name(ble), esp_err_to_name(pm));
  // A PM failure keeps normal companion operation, rather than disconnecting it.
  lastSample = millis();
}

void loop() {
  if (watchdogReady) esp_task_wdt_reset();
  const uint32_t now = millis();
  if (uint32_t(now - lastSample) < sampleIntervalMs) return;
  lastSample = now;
  const uint16_t mv = board.getBattMilliVolts();
  if (sustainedLow(mv, lowSamples)) {
    chargedSamples = 0;
    sleepForRecharge(true);
  }
}
}
#endif
