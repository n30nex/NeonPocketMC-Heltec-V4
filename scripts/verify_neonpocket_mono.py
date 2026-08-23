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
mesh = (ROOT / "examples/companion_radio/MyMesh.cpp").read_text(encoding="utf-8")
share_command = common_cli.index('strcmp(command, "gps advert share")')
gps_hardware_guard = common_cli.index("#if ENV_INCLUDE_GPS == 1", share_command)
if common_cli.index('strcmp(command, "gps advert prefs")') > share_command:
    raise SystemExit("saved-coordinate advert policy must not require physical GPS hardware")
if "_sensors->getLocationProvider() != NULL" not in common_cli[share_command:gps_hardware_guard]:
    raise SystemExit("live-location advert policy must require an actual GPS provider")
if common_cli.count("_prefs->advert_loc_policy = ADVERT_LOC_PREFS;") < 2:
    raise SystemExit("GPS-less CLI prefs must normalize saved live-location mode")
if "_sensors->getLocationProvider() == NULL" not in common_cli:
    raise SystemExit("CLI migration must test the actual GPS provider")
if mesh.count("sensors.getLocationProvider() == nullptr") < 2:
    raise SystemExit("GPS-less companion prefs must normalize live-location mode")

print("V4 NeonPocket mono contract verified")
