#if defined(NEONPOCKET_ULTIMATE) && defined(RCC6_WEB_AP)

#include "UltimateWebApi.h"

#include "MyMesh.h"
#include "UltimateOtaPublicKey.h"
#include "UltimateService.h"
#include <Ed25519.h>
#include <Update.h>
#include <helpers/esp32/SerialWebInterface.h>

namespace {
constexpr uint16_t ota_format_version = 1;
#ifndef NEONPOCKET_ULTIMATE_OTA_TARGET
  #define NEONPOCKET_ULTIMATE_OTA_TARGET "heltec_ultimate"
#endif
constexpr char ota_target[] = NEONPOCKET_ULTIMATE_OTA_TARGET;
constexpr char ota_mode[] = "web";

String jsonEscape(const char* text) {
  String output;
  if (!text) return output;
  output.reserve(strlen(text) + 8);
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; p++) {
    if (*p == '"' || *p == '\\') {
      output += '\\'; output += static_cast<char>(*p);
    } else if (*p == '\n') output += F("\\n");
    else if (*p == '\r') output += F("\\r");
    else if (*p == '\t') output += F("\\t");
    else if (*p >= 0x20) output += static_cast<char>(*p);
  }
  return output;
}

bool extractNumber(const String& body, const char* key, double& value) {
  const String needle = String('"') + key + F("\"");
  int at = body.indexOf(needle);
  if (at < 0 || (at = body.indexOf(':', at + needle.length())) < 0) return false;
  const char* start = body.c_str() + at + 1;
  char* end = nullptr;
  value = strtod(start, &end);
  return end != start;
}

bool extractBool(const String& body, const char* key, bool& value) {
  const String needle = String('"') + key + F("\"");
  int at = body.indexOf(needle);
  if (at < 0 || (at = body.indexOf(':', at + needle.length())) < 0) return false;
  const String tail = body.substring(at + 1);
  if (tail.startsWith("true")) { value = true; return true; }
  if (tail.startsWith("false")) { value = false; return true; }
  return false;
}

bool extractQuoted(const String& body, int& at, String& value) {
  at = body.indexOf('"', at);
  if (at < 0) return false;
  value = "";
  bool escaped = false;
  for (int i = at + 1; i < static_cast<int>(body.length()); i++) {
    const char c = body[i];
    if (escaped) {
      if (c == 'n') value += '\n';
      else if (c == 'r') value += '\r';
      else if (c == 't') value += '\t';
      else value += c;
      escaped = false;
    } else if (c == '\\') escaped = true;
    else if (c == '"') { at = i + 1; return true; }
    else value += c;
  }
  return false;
}

const char* otaResultName(UltimateWebApi::OtaResult result) {
  switch (result) {
    case UltimateWebApi::OtaResult::Success: return "success";
    case UltimateWebApi::OtaResult::Unauthorized: return "unauthorized";
    case UltimateWebApi::OtaResult::BadSignature: return "bad-signature";
    case UltimateWebApi::OtaResult::WrongTarget: return "wrong-target";
    case UltimateWebApi::OtaResult::HashMismatch: return "hash-mismatch";
    case UltimateWebApi::OtaResult::FlashError: return "flash-error";
    case UltimateWebApi::OtaResult::Aborted: return "aborted";
    case UltimateWebApi::OtaResult::BadPackage: return "bad-package";
    default: return "idle";
  }
}

const char* deliveryStateName(UltimateDeliveryState state) {
  switch (state) {
    case UltimateDeliveryState::Queued: return "queued";
    case UltimateDeliveryState::OnAir: return "on-air";
    case UltimateDeliveryState::Transmitted: return "transmitted";
    case UltimateDeliveryState::Acked: return "acked";
    case UltimateDeliveryState::NoAck: return "no-ack";
    case UltimateDeliveryState::Unconfirmed: return "unconfirmed";
    case UltimateDeliveryState::Failed: return "failed";
    default: return "idle";
  }
}

struct HistoryJsonContext {
  String* json;
  bool first;
};

bool appendHistoryJson(const UltimateHistoryRecord& record, void* raw) {
  auto& context = *static_cast<HistoryJsonContext*>(raw);
  String& json = *context.json;
  if (!context.first) json += ',';
  context.first = false;
  json += F("{\"sequence\":"); json += record.sequence;
  json += F(",\"timestamp\":"); json += record.timestamp;
  json += F(",\"incoming\":"); json += (record.flags & ULTIMATE_HISTORY_INCOMING) ? F("true") : F("false");
  json += F(",\"read\":"); json += (record.flags & ULTIMATE_HISTORY_READ) ? F("true") : F("false");
  json += F(",\"kind\":"); json += record.kind;
  json += F(",\"target\":"); json += record.target;
  json += F(",\"sender\":\""); json += jsonEscape(record.sender);
  json += F("\",\"text\":\""); json += jsonEscape(record.text);
  json += F("\",\"path\":"); json += record.path_len;
  json += F(",\"rssi\":"); json += record.rssi_dbm;
  json += F(",\"snr\":"); json += String(record.snr_quarter_db / 4.0f, 2);
  json += '}';
  return true;
}

struct HistoryExportContext {
  WebServer* server;
  uint16_t sent;
};

bool streamHistoryNdjson(const UltimateHistoryRecord& record, void* raw) {
  auto& context = *static_cast<HistoryExportContext*>(raw);
  String line = F("{\"sequence\":"); line += record.sequence;
  line += F(",\"timestamp\":"); line += record.timestamp;
  line += F(",\"incoming\":"); line += (record.flags & ULTIMATE_HISTORY_INCOMING) ? F("true") : F("false");
  line += F(",\"kind\":"); line += record.kind;
  line += F(",\"target\":"); line += record.target;
  line += F(",\"sender\":\""); line += jsonEscape(record.sender);
  line += F("\",\"text\":\""); line += jsonEscape(record.text); line += F("\"}\n");
  context.server->sendContent(line);
  if ((++context.sent & 7U) == 0) yield();
  return context.server->client().connected();
}
}

UltimateWebApi ultimate_web_api;

bool UltimateWebApi::authorized() {
  return interface && interface->authorizeHttpRequest();
}

void UltimateWebApi::sendJson(int status, const String& json) {
  server->sendHeader("Cache-Control", "no-store");
  server->send(status, "application/json", json);
}

void UltimateWebApi::begin(SerialWebInterface& web_interface) {
  interface = &web_interface;
  server = &web_interface.httpServer();
  server->on("/api/ultimate/status", HTTP_GET, [this]() { handleStatus(); });
  server->on("/api/ultimate/history", HTTP_GET, [this]() { handleHistory(); });
  server->on("/api/ultimate/export", HTTP_GET, [this]() { handleExport(); });
  server->on("/api/ultimate/history", HTTP_DELETE, [this]() { handleDeleteHistory(); });
  server->on("/api/ultimate/settings", HTTP_GET, [this]() { handleGetSettings(); });
  server->on("/api/ultimate/settings", HTTP_PUT, [this]() { handlePutSettings(); });
  server->on("/api/ultimate/location", HTTP_PUT, [this]() { handleLocation(); });
  server->on("/api/ultimate/ota", HTTP_POST,
      [this]() { handleOtaComplete(); }, [this]() { handleOtaUpload(); });
}

void UltimateWebApi::loop() {
  if (restart_at && static_cast<int32_t>(millis() - restart_at) >= 0) ESP.restart();
}

void UltimateWebApi::handleStatus() {
  if (!authorized()) return;
  const UltimateSnapshot& s = ultimate_service.getSnapshot();
  String json;
  json.reserve(8192);
  json = F("{\"version\":\""); json += NEONPOCKET_ULTIMATE_VERSION;
  json += F("\",\"git\":\""); json += ultimate_service.getBuildSha();
  json += F("\",\"uptime\":"); json += s.uptime_seconds;
  json += F(",\"rx\":"); json += s.rx_packets;
  json += F(",\"tx\":"); json += s.tx_packets;
  json += F(",\"txFailures\":"); json += s.tx_failures;
  json += F(",\"airtimeMs\":"); json += s.airtime_ms;
  json += F(",\"eventDrops\":"); json += s.event_drops;
  json += F(",\"freeHeap\":"); json += s.free_heap;
  json += F(",\"largestAllocation\":"); json += s.largest_allocation;
  json += F(",\"storageUsedKb\":"); json += s.storage_used_kb;
  json += F(",\"storageTotalKb\":"); json += s.storage_total_kb;
  json += F(",\"batteryMv\":"); json += s.battery_mv;
  json += F(",\"batteryTrendMvPerHour\":"); json += s.battery_trend_mv_per_hour;
  json += F(",\"batteryRuntimeMinutes\":"); json += s.battery_runtime_minutes;
  json += F(",\"batteryCapacityMah\":");
  json += ultimate_service.getSettings().battery_capacity_mah;
  json += F(",\"batteryProjectionValid\":");
  json += s.battery_projection_valid ? F("true") : F("false");
  json += F(",\"usbHostConnected\":");
  json += s.usb_host_connected ? F("true") : F("false");
  json += F(",\"historyCount\":"); json += s.history_count;
  json += F(",\"historyCapacity\":"); json += s.history_capacity;
  json += F(",\"unread\":"); json += s.unread_count;
  json += F(",\"queueDepth\":"); json += s.outbound_queue_depth;
  json += F(",\"displayFlushUs\":"); json += s.display_flush_micros;
  json += F(",\"displayFlushEmaUs\":"); json += s.display_flush_ema_micros;
  json += F(",\"displayTiles\":"); json += s.display_tiles_sent;
  json += F(",\"animationFrameMs\":"); json += s.animation_frame_millis;
  json += F(",\"displayTimeoutMs\":"); json += s.display_timeout_millis;
  json += F(",\"powerProfile\":"); json += ultimate_service.getSettings().power_profile;
  json += F(",\"memoryGate\":"); json += s.memory_gate_passed ? F("true") : F("false");
  json += F(",\"memoryGateLastPass\":"); json += s.memory_gate_last_pass_seconds;
  json += F(",\"signal\":");
  if (s.last_signal_valid) {
    json += F("{\"rssi\":"); json += s.last_rssi_dbm;
    json += F(",\"snr\":"); json += String(s.last_snr_quarter_db / 4.0f, 2); json += '}';
  } else json += F("null");

  const UltimateDeliverySnapshot& delivery = ultimate_service.getDelivery();
  json += F(",\"delivery\":{\"state\":\""); json += deliveryStateName(delivery.state);
  json += F("\",\"target\":\""); json += jsonEscape(delivery.target);
  json += F("\",\"roundTripMs\":"); json += delivery.round_trip_millis;
  json += F(",\"historySequence\":"); json += delivery.history_sequence;
  json += F(",\"ackExpected\":"); json += delivery.ack_expected ? F("true") : F("false");
  json += '}';

  json += F(",\"nodes\":[");
  for (uint8_t i = 0; i < ultimate_service.getNetworkCount(); i++) {
    const UltimateNetworkNode* node = ultimate_service.getNetworkNode(i);
    if (!node) continue;
    if (i) json += ',';
    json += F("{\"name\":\""); json += jsonEscape(node->name);
    json += F("\",\"role\":"); json += node->role;
    json += F(",\"path\":"); json += node->path_len;
    json += F(",\"seen\":"); json += node->last_seen;
    json += F(",\"packets\":"); json += node->packet_count;
    if (node->signal_attributable) {
      json += F(",\"rssi\":"); json += node->rssi_dbm;
      json += F(",\"snr\":"); json += String(node->snr_quarter_db / 4.0f, 2);
    }
    json += '}';
  }
  json += F("],\"minutes\":[");
  for (uint8_t i = 0; i < 120; i++) {
    uint32_t timestamp; uint16_t rx, tx, failures, battery; int16_t rssi;
    int8_t snr; uint8_t queue;
    if (!ultimate_service.getHighResolutionNewest(i, timestamp, rx, tx, failures,
                                                   rssi, snr, queue, battery)) break;
    if (i) json += ',';
    json += '['; json += timestamp; json += ','; json += rx; json += ','; json += tx;
    json += ','; json += failures; json += ','; json += rssi; json += ',';
    json += String(snr / 4.0f, 2); json += ','; json += queue; json += ','; json += battery;
    json += ']';
  }
  json += F("],\"hours\":[");
  bool first = true;
  for (uint16_t i = 0; i < 168; i++) {
    const UltimateMetricBucket* bucket = ultimate_service.getHourlyMetricNewest(i);
    if (!bucket) continue;
    if (!first) json += ','; first = false;
    json += '['; json += bucket->hour; json += ','; json += bucket->rx_packets;
    json += ','; json += bucket->tx_packets; json += ','; json += bucket->tx_failures;
    json += ','; json += bucket->battery_mv; json += ']';
  }
  json += F("]}");
  sendJson(200, json);
}

void UltimateWebApi::handleHistory() {
  if (!authorized()) return;
  const uint32_t before = server->hasArg("before") ? strtoul(server->arg("before").c_str(), nullptr, 10) : 0;
  uint16_t limit = server->hasArg("limit") ? server->arg("limit").toInt() : 20;
  if (limit < 1) limit = 1;
  if (limit > 50) limit = 50;
  String json;
  json.reserve(static_cast<size_t>(limit) * 260 + 16);
  json = F("{\"records\":[");
  HistoryJsonContext context = {&json, true};
  ultimate_service.visitHistory(before, limit, false, appendHistoryJson, &context);
  json += F("]}");
  sendJson(200, json);
}

void UltimateWebApi::handleExport() {
  if (!authorized()) return;
  server->sendHeader("Content-Disposition", "attachment; filename=neonpocket-history.ndjson");
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/x-ndjson", "");
  HistoryExportContext context = {server, 0};
  ultimate_service.visitHistory(0, 0, true, streamHistoryNdjson, &context);
  server->sendContent("");
}

void UltimateWebApi::handleDeleteHistory() {
  if (!authorized()) return;
  if (!server->hasHeader("X-NP-Confirm") || server->header("X-NP-Confirm") != "clear") {
    sendJson(409, F("{\"error\":\"confirmation-required\"}")); return;
  }
  const bool cleared = ultimate_service.clearHistory();
  sendJson(cleared ? 200 : 500,
           cleared ? F("{\"cleared\":true}") : F("{\"cleared\":false}"));
}

void UltimateWebApi::handleGetSettings() {
  if (!authorized()) return;
  const UltimateSettings& settings = ultimate_service.getSettings();
  String json = F("{\"historyCapacity\":"); json += settings.history_capacity;
  json += F(",\"scanCadenceMs\":"); json += settings.scan_cadence_ms;
  json += F(",\"privateNotifications\":"); json += settings.private_notifications ? F("true") : F("false");
  json += F(",\"batteryCalibrationMv\":"); json += settings.battery_calibration_mv;
  json += F(",\"batteryCapacityMah\":"); json += settings.battery_capacity_mah;
  json += F(",\"powerProfile\":"); json += settings.power_profile;
  json += F(",\"quickPhrases\":[");
  for (uint8_t i = 0; i < 8; i++) {
    if (i) json += ',';
    json += '"'; json += jsonEscape(settings.quick_phrases[i]); json += '"';
  }
  json += F("]}");
  sendJson(200, json);
}

void UltimateWebApi::handlePutSettings() {
  if (!authorized()) return;
  const String body = server->arg("plain");
  UltimateSettings updated = ultimate_service.getSettings();
  double number;
  bool boolean;
  if (extractNumber(body, "historyCapacity", number)) updated.history_capacity = number;
  if (extractNumber(body, "scanCadenceMs", number)) updated.scan_cadence_ms = number;
  if (extractNumber(body, "batteryCalibrationMv", number)) updated.battery_calibration_mv = number;
  if (extractNumber(body, "batteryCapacityMah", number)) {
    if (number != number || number < 0 || number > 20000 ||
        number != static_cast<uint16_t>(number) ||
        (number != 0 && (number < 50 || number > 20000))) {
      sendJson(400, F("{\"error\":\"invalid-battery-capacity\"}")); return;
    }
    updated.battery_capacity_mah = static_cast<uint16_t>(number);
  }
  if (extractNumber(body, "powerProfile", number)) updated.power_profile = number;
  if (extractBool(body, "privateNotifications", boolean)) updated.private_notifications = boolean;
  int at = body.indexOf("\"quickPhrases\"");
  if (at >= 0 && (at = body.indexOf('[', at)) >= 0) {
    for (uint8_t i = 0; i < 8; i++) {
      String phrase;
      if (!extractQuoted(body, at, phrase) || phrase.length() >= sizeof(updated.quick_phrases[i])) {
        sendJson(400, F("{\"error\":\"invalid-phrases\"}")); return;
      }
      phrase.toCharArray(updated.quick_phrases[i], sizeof(updated.quick_phrases[i]));
    }
  }
  if (!ultimate_service.updateSettings(updated)) {
    sendJson(400, F("{\"error\":\"invalid-settings\"}")); return;
  }
  handleGetSettings();
}

void UltimateWebApi::handleLocation() {
  if (!authorized()) return;
  const String body = server->arg("plain");
  double latitude, longitude, accuracy = 0;
  if (!extractNumber(body, "latitude", latitude) || !extractNumber(body, "longitude", longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
    sendJson(400, F("{\"error\":\"invalid-location\"}")); return;
  }
  extractNumber(body, "accuracy", accuracy);
  bool advertise = false;
  const bool has_advertise = extractBool(body, "advertise", advertise);
  sensors.node_lat = latitude;
  sensors.node_lon = longitude;
  if (has_advertise) {
    the_mesh.getNodePrefs()->advert_loc_policy = advertise ? ADVERT_LOC_PREFS : ADVERT_LOC_NONE;
  }
  the_mesh.savePrefs();
  String json = F("{\"saved\":true,\"latitude\":"); json += String(latitude, 6);
  json += F(",\"longitude\":"); json += String(longitude, 6);
  json += F(",\"accuracy\":"); json += String(accuracy, 1);
  json += F(",\"advertise\":");
  json += (the_mesh.getNodePrefs()->advert_loc_policy == ADVERT_LOC_NONE ? F("false") : F("true"));
  json += '}';
  sendJson(200, json);
}

void UltimateWebApi::failOta(OtaResult result) {
  ota_result = result;
  if (ota_update_started) Update.abort();
  ota_update_started = false;
}

bool UltimateWebApi::validateOtaHeader() {
  if (memcmp(ota_header.magic, "NPU2", 4) != 0 ||
      ota_header.format_version != ota_format_version ||
      ota_header.header_size != sizeof(UltimateOtaHeader) ||
      ota_header.application_length == 0) {
    failOta(OtaResult::BadPackage); return false;
  }
  if (strncmp(ota_header.target, ota_target, sizeof(ota_header.target)) != 0 ||
      strncmp(ota_header.mode, ota_mode, sizeof(ota_header.mode)) != 0) {
    failOta(OtaResult::WrongTarget); return false;
  }
  const size_t signed_length = offsetof(UltimateOtaHeader, signature);
  if (!Ed25519::verify(ota_header.signature, ULTIMATE_OTA_PUBLIC_KEY,
                                      reinterpret_cast<const uint8_t*>(&ota_header), signed_length)) {
    failOta(OtaResult::BadSignature); return false;
  }
  if (!Update.begin(ota_header.application_length, U_FLASH)) {
    failOta(OtaResult::FlashError); return false;
  }
  ota_hash.reset();
  ota_update_started = true;
  return true;
}

bool UltimateWebApi::writeOtaPayload(const uint8_t* data, size_t length) {
  if (!ota_update_started || ota_payload_bytes + length > ota_header.application_length) {
    failOta(OtaResult::BadPackage); return false;
  }
  ota_hash.update(data, length);
  if (Update.write(const_cast<uint8_t*>(data), length) != length) {
    failOta(OtaResult::FlashError); return false;
  }
  ota_payload_bytes += length;
  return true;
}

void UltimateWebApi::handleOtaUpload() {
  HTTPUpload& upload = server->upload();
  if (upload.status == UPLOAD_FILE_START) {
    ota_header = {};
    ota_header_bytes = ota_payload_bytes = 0;
    ota_update_started = false;
    ota_result = authorized() ? OtaResult::Receiving : OtaResult::Unauthorized;
  } else if (upload.status == UPLOAD_FILE_WRITE && ota_result == OtaResult::Receiving) {
    const uint8_t* data = upload.buf;
    size_t length = upload.currentSize;
    if (ota_header_bytes < sizeof(ota_header)) {
      const size_t take = min(length, sizeof(ota_header) - ota_header_bytes);
      memcpy(reinterpret_cast<uint8_t*>(&ota_header) + ota_header_bytes, data, take);
      ota_header_bytes += take; data += take; length -= take;
      if (ota_header_bytes == sizeof(ota_header) && !validateOtaHeader()) return;
    }
    if (length) writeOtaPayload(data, length);
  } else if (upload.status == UPLOAD_FILE_END && ota_result == OtaResult::Receiving) {
    if (!ota_update_started || ota_payload_bytes != ota_header.application_length) {
      failOta(OtaResult::BadPackage); return;
    }
    uint8_t digest[32]; ota_hash.finalize(digest, sizeof(digest));
    if (memcmp(digest, ota_header.application_sha256, sizeof(digest)) != 0) {
      failOta(OtaResult::HashMismatch); return;
    }
    if (!Update.end(false)) { failOta(OtaResult::FlashError); return; }
    ota_update_started = false;
    ota_result = OtaResult::Success;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    failOta(OtaResult::Aborted);
  }
}

void UltimateWebApi::handleOtaComplete() {
  const OtaResult result = ota_result;
  const int status = result == OtaResult::Success ? 200 :
                     result == OtaResult::Unauthorized ? 401 : 400;
  String json = F("{\"result\":\""); json += otaResultName(result); json += F("\"}");
  sendJson(status, json);
  if (result == OtaResult::Success) restart_at = millis() + 900;
  ota_result = OtaResult::Idle;
}

#endif
