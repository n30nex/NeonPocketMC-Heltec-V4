<p align="center">
  <img src="https://raw.githubusercontent.com/n30nex/NeonPocketMC/main/branding/neonpocketmc-mark.png" alt="NeonPocketMC logo" width="140">
</p>

# NeonPocketMC for Heltec V4

Experimental MeshCore BLE or native-USB companion firmware for the **Heltec WiFi LoRa 32 V4/V4.3 with its integrated 128×64 SSD1306 OLED and SX1262**.

> [!WARNING]
> **Standard integrated-OLED Heltec V4/V4.3 only. Do not flash this on Heltec V3, the V4 TFT expansion build, RCC6, RC52, Wireless Tracker, or another hardware variant.** This repository ships separate BLE and native-USB companion images—no Wi-Fi, repeater, or room-server firmware.

**Guided install:** [flasher.canadaverse.org](https://flasher.canadaverse.org/)

## What is included

- MeshCore 1.17.1 receive-gain fix line, based on exact upstream commit `727fc0512ce08bfd7b499e46daa7fca6eeec730d`.
- Upstream V4/V4.3 hardware handling retained intact: runtime GC1109/KCT8103L FEM support, VFEM power sequencing, SX1262 register `0x8B5` receive patch, DIO2 RF switching, 1.8 V DIO3 TCXO, active-high Vext, battery sensing, and integrated OLED.
- A smooth procedural NeonPocket startup, compact phone-like Home dashboard, Inbox, Nearby, Radio, Bluetooth, Advert, and confirmed Power pages.
- 350 contacts, 40 channels, and 256 pending companion frames.
- 60-second OLED timeout while BLE and LoRa continue running.
- Fail-closed storage mounting. Existing identity, contacts, channels, and preferences are not silently formatted after a mount error.

The firmware value `TX 10 dBm` is intentional on this board: MeshCore’s V4 hardware layer drives the external PA and Heltec documents the board for substantially higher final RF output. Do not copy V3 power values into V4.

## Button controls

- Screen off: the first gesture only wakes the OLED.
- Single press: next page or next Inbox message.
- Double press: current-page action; on Home it opens Inbox.
- Triple press: return to Home; from Inbox it also clears every local unread message.
- Hold: show Power confirmation.
- Hold again within eight seconds while confirmation is visible: hibernate after release.
- During the first eight seconds after boot, hold enters MeshCore CLI Rescue.

## Install

Download the current experimental [`v1.0.0-rc.3` release](https://github.com/n30nex/NeonPocketMC-Heltec-V4/releases/tag/v1.0.0-rc.3).

- Normal update: flash `NeonPocketMC-Heltec-V4-BLE-app.bin` at offset `0x10000`.
- Wired companion: flash `NeonPocketMC-Heltec-V4-USB-app.bin` at offset `0x10000`.
- BLE recovery: flash `NeonPocketMC-Heltec-V4-BLE-recovery-preserves-settings.bin` at offset `0x0`.
- USB recovery: flash `NeonPocketMC-Heltec-V4-USB-recovery-preserves-settings.bin` at offset `0x0`.

The recovery image replaces boot/application metadata and clears ESP32 NVS state such as old BLE bonds, but does not extend into the SPIFFS data partition that holds MeshCore identity and preferences. Always verify the published SHA-256 manifest before flashing, attach a suitable antenna before transmitting, and use USB recovery if an experimental candidate fails to boot.

## Hardware gallery

These frames were exported from the SSD1306 framebuffer of the connected V4 qualification unit. They are actual device output, not browser mockups.

<p align="center">
  <img src="docs/images/neonpocket-v4-splash-on-device.gif" alt="NeonPocketMC demo-scene startup captured from the Heltec V4 OLED" width="640">
</p>

<table>
  <tr>
    <td align="center"><img src="docs/images/neonpocket-v4-home.png" alt="Heltec V4 Home dashboard" width="360"><br><strong>Home</strong></td>
    <td align="center"><img src="docs/images/neonpocket-v4-nearby.png" alt="Heltec V4 Nearby page" width="360"><br><strong>Nearby</strong></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/neonpocket-v4-radio.png" alt="Heltec V4 Radio page" width="360"><br><strong>Radio</strong></td>
    <td align="center"><img src="docs/images/neonpocket-v4-bluetooth.png" alt="Heltec V4 Bluetooth page" width="360"><br><strong>Bluetooth</strong></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/neonpocket-v4-advert.png" alt="Heltec V4 Advert page" width="360"><br><strong>Advert</strong></td>
    <td align="center"><img src="docs/images/neonpocket-v4-power.png" alt="Heltec V4 Power page" width="360"><br><strong>Power</strong></td>
  </tr>
</table>

Capture provenance and checksums are recorded in [`docs/images/README.md`](docs/images/README.md).

## Status

`v1.0.0-rc.3` is experimental. It adds native USB and retains the RC2 triple-press Home/clear shortcut. Both companion targets are CI-qualified; no new physical V4 receipt is claimed for RC3.

## Upstream and licensing

This is a community project, not an official Heltec or MeshCore release. It is derived from [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore). Upstream and third-party license files remain in this source tree.
