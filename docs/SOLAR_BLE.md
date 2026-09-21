# Heltec V4.2 solar BLE companion candidate

Target: `heltec_v4_solar_companion_ble`. Firmware identity:
`v1.17.1-v42-solar-ble-rc.1`. This is a standard MeshCore BLE companion profile,
with the usual MeshCore/MeshMapper protocol and standard companion screens.
It is a bench-test candidate, not a physically qualified winter deployment.

## Intended installation

- Heltec WiFi LoRa 32 V4.2, original 16 MB flash / 2 MB PSRAM / GC1109 RF front end.
- HXJNLDC 755590, nominal 3.7 V / 5000 mAh / 18.5 Wh LiPo on the battery connector.
- Two 6 W panels through a regulated **5 V combined output** to the solar connector.
  The board must receive 5 V, not 10 V from two 5 V outputs wired in series.
- Exact polarity must be checked. Connector shape alone does not establish polarity.
- V4 R8, V3, and expansion-kit targets are excluded from this candidate's scope.

## Operating behavior

- LoRa stays in continuous receive during normal operation; no RX duty cycling.
- BLE remains discoverable with 250-300 ms advertising intervals. There are no
  long BLE-off windows. Actual app connection latency depends on phone scanning,
  bonding, RF conditions, and the app; the interval is not a latency guarantee.
- Bluetooth controller modem sleep and 40-80 MHz CPU frequency scaling reduce
  idle overhead. CPU light sleep is deliberately disabled pending physical IRQ,
  USB, and packet-reception qualification; this build never blindly sleeps the
  processor while the BLE stack is active.
- The OLED turns off after 15 seconds. Existing LoRa settings, TX power, identity,
  contacts, channels, and user settings are retained.
- An existing saved BLE PIN is retained. If none is saved, the first random PIN
  is saved once so recharge/restarts do not require finding a new PIN on the roof.
- Three consecutive battery readings below **3.40 V**, sampled eight seconds
  apart, enter recharge sleep. A brief transmit-related voltage sag does not
  immediately shut the unit down.
- Recharge sleep disables the RF front end, holds the radio in reset, switches
  off the display supply, and always arms a **60-second wake timer**. The unit
  checks voltage before starting the display, radio, BLE, or storage.
- Normal operation resumes after **two successive wake checks at or above
  3.70 V**. A brownout enters the same recovery qualification. Battery calibration
  is carried through timed sleep. Invalid readings cannot qualify as recharged.
- UI power-off also uses timed recovery; there is no intentional permanent-off
  state in this target. Fatal startup errors retry rather than waiting for a button.
- A 30-second task watchdog covers startup after board initialization and the
  main loop. Watchdog resets use the same battery recovery qualification.

LoRa and BLE are unavailable during emergency recharge sleep. Keeping the radio
receiving after the battery is exhausted is not physically possible. Volatile
pending frames do not survive deep sleep; this is not an overnight packet logger.
No solar-present GPIO is fitted: recovery is based on battery voltage and a timer,
not an invented USB/solar detector. RTC recovery state is lost after total power
loss; the low-voltage boot check still runs on the next power-on.

## Charging and winter limits

The V4.2 schematic specifies a CN3165 charger with a 2.2 kOhm current-setting
resistor, approximately **540 mA**. On the solar connector the running radio's
load shares that charging path. A 12 W panel rating does not bypass the charger
limit. Refilling an empty 5000 mAh battery takes more than 9.3 equivalent hours
at 540 mA after load and charge taper are included; replacing one night's use
requires less, depending on measured current and actual sunlight.

The V4.2 charger TEMP input is grounded in Heltec's schematic. Firmware cannot
provide battery-temperature charge inhibition through that wiring. Do not rely
on this candidate to make below-freezing LiPo charging safe. The exact HXJNLDC
pack's manufacturer charge-temperature specification and cold-charge protection
remain unverified; independent cell-temperature protection is required before
outdoor winter charging unless the pack itself is qualified for those conditions.

No overnight-runtime, daily-full-charge, or failure-free claim has been measured.
The nominal capacity, cold-temperature capacity, starting charge, battery health,
local traffic, TX power, panel shading, and daylight all affect the result.

## Install and qualify before roof mounting

Use the Actions-built `NeonPocketMC-Heltec-V4-Solar-BLE-app.bin` at **0x10000** for
the normal preserving update. Check `SHA256SUMS.txt` first. The recovery image at
0x0 is for documented recovery only: it replaces boot/application metadata and
may clear NVS/BLE bonds. Preserve a full device backup before any recovery flash.
No firmware has been flashed or added to the public flasher by creating this PR.

1. Start indoors with a charged battery and a tuned antenna. Confirm the reported
   board, firmware identity, saved identity/settings, and battery reading against
   a multimeter. Pair both apps before enclosing the board; record the saved PIN.
2. USB startup output must report `BLE modem sleep=ESP_OK` and
   `CPU 40-80 MHz=ESP_OK`, plus `task watchdog=ready`. A power-management failure leaves normal BLE running
   and reports the error; do not claim power savings from that unit.
3. Measure battery-side current with screen off, while disconnected, connected,
   receiving, and transmitting. Compare received packets with the stock image.
4. Exercise at least 100 phone connect/disconnect cycles and measure foreground
   MeshMapper and MeshCore reconnect times. Verify bonds and the PIN after reboot.
5. With a protected battery simulator/current-limited bench supply, test low
   voltage, recharge, complete loss/restoration, slow ramps, and flickering input.
   Confirm 60-second retries, the two-read resume threshold, and recovery without
   touching Reset/PRG. Never deliberately over-discharge the real LiPo for a test.
6. Run an overnight receive/phone-reconnect soak and record the following day's
   charge balance. Verify the charger blocks unsafe cell temperatures in hardware.

## Reproducibility and attribution

The solar-only target uses IoTThinks' PowerSaving Arduino Core 2.0.17 (ESP-IDF
4.4.7), also used by the ULP product family. Its published archive is verified
before CI compilation:
`48b6ef1fd5b45d4560ea35672dd7e7f10a1c7157d30e70a63dcfba1c984e0936`.
Other targets keep their existing framework. Retain the upstream licenses in
the source archive and the framework package. Battery-policy tests run with
`pio test -e native`; the standard V4 BLE build remains a regression target.

Sources: [Heltec V4.2 schematic](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic/WiFi_LoRa_32_V4.2.pdf),
[power-saving core](https://github.com/IoTThinks/powersaving-arduino-lib),
[Espressif Bluetooth sleep API](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/bluetooth/controller_vhci.html#_CPPv419esp_bt_sleep_enablev).
