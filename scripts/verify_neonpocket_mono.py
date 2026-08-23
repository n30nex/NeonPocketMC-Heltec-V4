#!/usr/bin/env python3
"""Small release contract check for the V4 NeonPocket target."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [needle for needle in needles if needle not in text]
    if missing:
        raise SystemExit(f"{path}: missing {missing}")


require(
    "variants/heltec_v4/platformio.ini",
    "[env:heltec_v4_neonpocket_companion_ble]",
    "extends = heltec_v4_oled",
    "-D DISPLAY_CLASS=SSD1306Display",
    "-D DISPLAY_REQUIRED=1",
    "-D NEONPOCKET_MONO_UI=1",
    "-D NEONPOCKET_DEVICE_LABEL='\"HELTEC V4 BLE\"'",
    "-D NEONPOCKET_SAFE_SPIFFS_BOOTSTRAP=1",
    "-D AUTO_OFF_MILLIS=60000",
    "-D OFFLINE_QUEUE_SIZE=256",
    "-D FIRMWARE_VERSION='\"v1.17.1\"'",
    "-D P_LORA_PA_POWER=7",
    "-D P_LORA_GC1109_PA_EN=2",
    "-D P_LORA_KCT8103L_PA_CSD=2",
    "-D SX126X_REGISTER_PATCH=1",
    "-D LORA_TX_POWER=10",
)
require(
    "examples/companion_radio/ui-new/NeonPocketMono.h",
    "DURATION_MILLIS = 3200",
    "FRAME_MILLIS = 80",
    "NEONPOCKET",
)
require(
    "examples/companion_radio/main.cpp",
    "SPIFFS.begin(false)",
    "isSpiffsPartitionErased",
    "data was not formatted",
    "required display initialization failed",
)
require(
    "examples/companion_radio/ui-new/UITask.cpp",
    "isPowerConfirmationVisible",
    "gotoMsgPreviewScreen",
    "neonGoHome",
    "curr == msg_preview",
    "handleTripleClick",
    "HOLD AGAIN",
)

common_cli = (ROOT / "src/helpers/CommonCLI.cpp").read_text(encoding="utf-8")
if common_cli.index('strcmp(command, "gps advert prefs")') > \
        common_cli.index("#if ENV_INCLUDE_GPS == 1"):
    raise SystemExit("saved-coordinate advert policy must not require physical GPS hardware")

print("V4 NeonPocket mono contract verified")
