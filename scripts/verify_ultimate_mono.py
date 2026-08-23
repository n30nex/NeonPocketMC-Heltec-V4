#!/usr/bin/env python3
"""Static contract for the Heltec OLED Ultimate companion targets."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


board = ""
platform_path = ""
for candidate in ("v3", "v4"):
    candidate_path = f"variants/heltec_{candidate}/platformio.ini"
    candidate_text = text(candidate_path)
    if f"[env:heltec_{candidate}_ultimate_companion_ble]" in candidate_text:
        board = candidate
        platform_path = candidate_path
        break
if not board:
    raise SystemExit("Ultimate verifier supports only Heltec V3/V4 repositories")

platform = text(platform_path)
main = text("examples/companion_radio/main.cpp")
mesh_h = text("examples/companion_radio/MyMesh.h")
mesh_cpp = text("examples/companion_radio/MyMesh.cpp")
service_h = text("examples/companion_radio/UltimateService.h")
service_cpp = text("examples/companion_radio/UltimateService.cpp")
web = text("examples/companion_radio/UltimateWebApi.cpp")
ui = text("examples/companion_radio/ui-new/UITask.cpp")
asset = text("examples/companion_radio/webui/Rcc6WebUiAssets.h")

ble_env = f"[env:heltec_{board}_ultimate_companion_ble]"
web_env = f"[env:heltec_{board}_ultimate_companion_web]"
ota_target = f"-D NEONPOCKET_ULTIMATE_OTA_TARGET='\"heltec_{board}\"'"

checks = {
    "Ultimate BLE target": ble_env in platform,
    "Ultimate Web target": web_env in platform,
    "MeshCore 1.17.1": "-D FIRMWARE_VERSION='\"v1.17.1\"'" in platform,
    "mutually exclusive transports": platform.index(ble_env) != platform.index(web_env)
        and "-D BLE_PIN_CODE=123456" in platform[platform.index(ble_env):platform.index(web_env)]
        and "-D RCC6_WEB_AP=1" in platform[platform.index(web_env):],
    "OLED NeonPocket renderer": "-D NEONPOCKET_MONO_UI=1" in platform,
    "32 KiB runtime gate": "-D NEONPOCKET_MEMORY_GATE_BYTES=32768" in platform,
    "capacity profile": all(token in platform for token in (
        "-D MAX_CONTACTS=350", "-D MAX_GROUP_CHANNELS=40", "-D OFFLINE_QUEUE_SIZE=256")),
    "target-specific OTA marker": ota_target in platform,
    "fail-closed storage": "beginSpiffsPreservingData()" in main
        and "Ultimate storage initialization failed; data preserved" in main,
    "service lifecycle": "ultimate_service.begin(SPIFFS" in main
        and "ultimate_service.loop();" in main,
    "Web lifecycle": "web_interface.begin(" in main and "ultimate_web_api.begin(" in main,
    "256-byte journal": "static_assert(sizeof(UltimateHistoryRecord) == 256" in service_h,
    "bounded event and network caches": "pending_capacity = 32" in service_h
        and "network_capacity = 64" in service_h,
    "history retention choices": "capacity == 128 || capacity == 512 || capacity == 2048" in service_cpp,
    "message composer": "sendUltimateDirect" in mesh_h and "sendUltimateChannel" in mesh_h
        and "UltimateComposerState" in service_h,
    "safe hop decoding": "encoded_path_len & 0x3F" in service_cpp
        and "encoded_path_len & 0x3F" in mesh_cpp,
    "OLED Network and Ultimate pages": "HomePage::NETWORK" in ui and "HomePage::ULTIMATE" in ui,
    "flood advert action": "the_mesh.advert(" in ui and "true" in ui,
    "authenticated Ultimate APIs": "/api/ultimate/status" in web
        and "/api/ultimate/history" in web and "/api/ultimate/location" in web,
    "generated embedded WebUI": "RCC6_WEB_UI_INDEX_GZ[] PROGMEM" in asset,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Ultimate OLED contract failed: " + ", ".join(failed))
print(f"Heltec {board.upper()} Ultimate OLED contract verified")
