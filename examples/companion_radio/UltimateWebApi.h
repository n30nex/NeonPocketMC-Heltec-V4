#pragma once

#if defined(NEONPOCKET_ULTIMATE) && defined(RCC6_WEB_AP)

#include <Arduino.h>
#include <SHA256.h>
#include <WebServer.h>

class SerialWebInterface;

#pragma pack(push, 1)
struct UltimateOtaHeader {
  char magic[4];
  uint16_t format_version;
  uint16_t header_size;
  char target[16];
  char mode[8];
  char version[16];
  char git_sha[41];
  uint32_t application_length;
  uint8_t application_sha256[32];
  uint8_t signature[64];
};
#pragma pack(pop)

class UltimateWebApi {
public:
  enum class OtaResult : uint8_t {
    Idle, Receiving, Success, Unauthorized, BadPackage, BadSignature,
    WrongTarget, HashMismatch, FlashError, Aborted
  };

private:
  SerialWebInterface* interface = nullptr;
  WebServer* server = nullptr;
  UltimateOtaHeader ota_header = {};
  SHA256 ota_hash;
  OtaResult ota_result = OtaResult::Idle;
  size_t ota_header_bytes = 0;
  size_t ota_payload_bytes = 0;
  bool ota_update_started = false;
  uint32_t restart_at = 0;

  bool authorized();
  void sendJson(int status, const String& json);
  void handleStatus();
  void handleHistory();
  void handleExport();
  void handleDeleteHistory();
  void handleGetSettings();
  void handlePutSettings();
  void handleLocation();
  void handleOtaComplete();
  void handleOtaUpload();
  bool validateOtaHeader();
  bool writeOtaPayload(const uint8_t* data, size_t length);
  void failOta(OtaResult result);

public:
  void begin(SerialWebInterface& web_interface);
  void loop();
};

extern UltimateWebApi ultimate_web_api;

#endif
