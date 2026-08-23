#ifdef RCC6_WEB_AP

#include "SerialWebInterface.h"
#include <Preferences.h>
#include <esp_system.h>
#include <errno.h>
#include <lwip/sockets.h>
#define RCC6_WEB_UI_ASSETS_IMPLEMENTATION
#include <Rcc6WebUiAssets.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr char PASSWORD_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz";
constexpr char HTTP_SESSION_HEADER[] = "X-RCC6-Session";
constexpr char HTTP_ACK_HEADER[] = "X-RCC6-Ack";
constexpr char HTTP_SEQUENCE_HEADER[] = "X-RCC6-Seq";
constexpr char HTTP_AUTH_USER[] = "meshcore";

bool isSafeSsidChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool isAllowedHttpOpcode(uint8_t opcode) {
  switch (opcode) {
    case 1:   // AppStart
    case 2:   // SendTxtMsg
    case 3:   // SendChannelTxtMsg
    case 4:   // GetContacts
    case 7:   // SendSelfAdvert
    case 10:  // SyncNextMessage
    case 20:  // GetBatteryVoltage
    case 22:  // DeviceQuery
    case 31:  // GetChannel
    case 56:  // GetStats
      return true;
    default:
      return false;
  }
}

String jsonEscape(const char* value) {
  String escaped;
  if (value == nullptr) return escaped;
  escaped.reserve(strlen(value) + 8);
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(value); *p != 0; p++) {
    if (*p == '"' || *p == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(*p);
    } else if (*p < 0x20) {
      escaped += "\\u00";
      escaped += HEX_DIGITS[*p >> 4];
      escaped += HEX_DIGITS[*p & 0x0f];
    } else {
      escaped += static_cast<char>(*p);
    }
  }
  return escaped;
}
}

void SerialWebInterface::FrameQueue::clear() {
  head = 0;
  count = 0;
}

bool SerialWebInterface::FrameQueue::full() const {
  return count == FRAME_QUEUE_SIZE;
}

bool SerialWebInterface::FrameQueue::empty() const {
  return count == 0;
}

bool SerialWebInterface::FrameQueue::push(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 1 || len > MAX_FRAME_SIZE || full()) return false;

  Frame& frame = frames[(head + count) % FRAME_QUEUE_SIZE];
  frame.len = static_cast<uint16_t>(len);
  memcpy(frame.data, data, len);
  count++;
  return true;
}

SerialWebInterface::Frame* SerialWebInterface::FrameQueue::front() {
  return empty() ? nullptr : &frames[head];
}

void SerialWebInterface::FrameQueue::pop() {
  if (empty()) return;
  head = (head + 1) % FRAME_QUEUE_SIZE;
  count--;
}

size_t SerialWebInterface::FrameQueue::pop(uint8_t* dest) {
  Frame* frame = front();
  if (frame == nullptr || dest == nullptr) return 0;

  const size_t len = frame->len;
  memcpy(dest, frame->data, len);
  pop();
  return len;
}

SerialWebInterface::SerialWebInterface()
  : _initialized(false), _enabled(false), _routes_registered(false),
    _tcp_connected(false), _tcp_port(5000), _ap_ssid{}, _ap_password{},
    _prefer_station(false), _station_active(false), _fallback_active(false),
    _station_ssid{}, _station_password{}, _current_ip(), _restart_at(0),
    _tcp_server(), _tcp_client(), _http_server(80), _session(Session::NONE),
    _http_remote(), _http_session_id{}, _http_last_activity(0),
    _http_inflight_seq(0), _http_next_seq(1), _recv_queue{}, _send_queue{},
    _tcp_rx_state(TcpRxState::MARKER), _tcp_rx_length(0), _tcp_rx_offset(0),
    _tcp_rx_data{}, _tcp_tx_data{}, _tcp_tx_length(0), _tcp_tx_offset(0),
    _http_post_result(HttpPostResult::NONE), _http_post_expected(0),
    _http_post_length(0), _http_post_data{} {
  _recv_queue.clear();
  _send_queue.clear();
}

void SerialWebInterface::begin(const char* node_name, uint16_t tcp_port) {
  _tcp_port = tcp_port == 0 ? 5000 : tcp_port;
  configureIdentity(node_name);
  registerHttpRoutes();
  _initialized = true;
}

IPAddress SerialWebInterface::getApIP() const {
  return IPAddress(192, 168, 4, 1);
}

const char* SerialWebInterface::getCurrentSsid() const {
  return _station_active ? _station_ssid : _ap_ssid;
}

void SerialWebInterface::configureIdentity(const char* node_name) {
  char suffix[24] = {};
  size_t out = 0;
  bool last_was_separator = false;

  if (node_name != nullptr) {
    for (size_t i = 0; node_name[i] != '\0' && out < sizeof(suffix) - 1; i++) {
      const char c = node_name[i];
      if (isSafeSsidChar(c)) {
        suffix[out++] = c;
        last_was_separator = false;
      } else if (out > 0 && !last_was_separator) {
        suffix[out++] = '-';
        last_was_separator = true;
      }
    }
  }
  while (out > 0 && suffix[out - 1] == '-') out--;
  suffix[out] = '\0';
  if (out == 0) strcpy(suffix, "RCC6");
  snprintf(_ap_ssid, sizeof(_ap_ssid), "MeshCore-%s", suffix);

  Preferences preferences;
  String password;
  String station_ssid;
  String station_password;
  const bool opened = preferences.begin("rcc6-web", false);
  if (opened) {
    if (preferences.isKey("ap-pass")) password = preferences.getString("ap-pass", "");
    if (preferences.isKey("sta-mode")) _prefer_station = preferences.getBool("sta-mode", false);
    if (preferences.isKey("sta-ssid")) station_ssid = preferences.getString("sta-ssid", "");
    if (preferences.isKey("sta-pass")) station_password = preferences.getString("sta-pass", "");
  }

  if (password.length() != sizeof(_ap_password) - 1) {
    for (size_t i = 0; i < sizeof(_ap_password) - 1; i++) {
      _ap_password[i] = PASSWORD_ALPHABET[esp_random() % (sizeof(PASSWORD_ALPHABET) - 1)];
    }
    _ap_password[sizeof(_ap_password) - 1] = '\0';
    if (opened) preferences.putString("ap-pass", _ap_password);
  } else {
    password.toCharArray(_ap_password, sizeof(_ap_password));
  }

  const bool valid_station = station_ssid.length() >= 1 && station_ssid.length() <= 32 &&
      (station_password.length() == 0 ||
       (station_password.length() >= 8 && station_password.length() <= 64));
  if (valid_station) {
    station_ssid.toCharArray(_station_ssid, sizeof(_station_ssid));
    station_password.toCharArray(_station_password, sizeof(_station_password));
  } else {
    _prefer_station = false;
    _station_ssid[0] = '\0';
    _station_password[0] = '\0';
  }

  if (opened) preferences.end();
}

void SerialWebInterface::registerHttpRoutes() {
  if (_routes_registered) return;

  static const char* collected_headers[] = {
      HTTP_SESSION_HEADER, HTTP_ACK_HEADER
#ifdef NEONPOCKET_ULTIMATE
      , "X-NP-Confirm"
#endif
  };
  _http_server.collectHeaders(collected_headers,
      sizeof(collected_headers) / sizeof(collected_headers[0]));
  _http_server.on("/", HTTP_GET, [this]() { handleIndex(); });
  _http_server.on("/index.html", HTTP_GET, [this]() { handleIndex(); });
  _http_server.on("/api/frame", HTTP_GET, [this]() { handleHttpGetFrame(); });
  _http_server.on("/api/frame", HTTP_POST,
      [this]() { handleHttpPostFrame(); },
      [this]() { handleHttpPostRaw(); });
  _http_server.on("/api/network", HTTP_GET, [this]() { handleHttpGetNetwork(); });
  _http_server.on("/api/network", HTTP_POST, [this]() { handleHttpPostNetwork(); });
  _http_server.onNotFound([this]() {
    if (httpAuthorized()) sendHttpStatus(404);
  });
  _routes_registered = true;
}

bool SerialWebInterface::startAccessPoint() {
  WiFi.mode(WIFI_AP);
  const IPAddress ap_ip = getApIP();
  if (!WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(_ap_ssid, _ap_password, 1, false, 1)) {
    WiFi.softAPdisconnect(true);
    Serial.println("Web AP: failed to start");
    return false;
  }

  _station_active = false;
  _current_ip = ap_ip;
  return true;
}

void SerialWebInterface::startServers() {
  _tcp_server.begin(_tcp_port);
  _tcp_server.setNoDelay(true);
  _http_server.begin();
  _enabled = true;
}

void SerialWebInterface::enable() {
  if (_enabled || !_initialized) return;

  clearSession();
  _station_active = false;
  _fallback_active = false;
  _current_ip = IPAddress();

  if (_prefer_station && _station_ssid[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(_station_ssid, _station_password);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED &&
        static_cast<uint32_t>(millis() - started) < STA_CONNECT_TIMEOUT_MS) {
      delay(50);
    }

    const IPAddress station_ip = WiFi.localIP();
    const bool has_ip = station_ip[0] != 0 || station_ip[1] != 0 ||
        station_ip[2] != 0 || station_ip[3] != 0;
    if (WiFi.status() == WL_CONNECTED && has_ip) {
      _station_active = true;
      _current_ip = station_ip;
      startServers();
      Serial.printf("Web WiFi: %s at %s (HTTP, TCP/%u)\n",
          _station_ssid, station_ip.toString().c_str(), _tcp_port);
      return;
    }

    Serial.printf("Web WiFi: failed to join %s; starting setup AP\n", _station_ssid);
    WiFi.disconnect(true, false);
    if (!saveNetworkConfig(false, String(), String())) {
      _prefer_station = false;
      _station_ssid[0] = '\0';
      _station_password[0] = '\0';
      Serial.println("Web WiFi: failed to persist setup AP fallback");
    }
    _fallback_active = true;
  }

  if (!startAccessPoint()) return;
  startServers();
  Serial.printf("Web AP: %s at %s (HTTP, TCP/%u)%s\n",
      _ap_ssid, _current_ip.toString().c_str(), _tcp_port,
      _fallback_active ? " [station fallback]" : "");
}

void SerialWebInterface::disable() {
  if (!_enabled) return;

  _enabled = false;
  _http_server.stop();
  _tcp_server.end();
  clearSession();
  if (_station_active) {
    WiFi.disconnect(true, false);
  } else {
    WiFi.softAPdisconnect(true);
  }
  _station_active = false;
  _fallback_active = false;
  _current_ip = IPAddress();
}

bool SerialWebInterface::isConnected() const {
  if (!_enabled) return false;
  if (_session == Session::TCP) return _tcp_connected;
  if (_session == Session::HTTP) {
    return static_cast<uint32_t>(millis() - _http_last_activity) < HTTP_SESSION_TIMEOUT_MS;
  }
  return false;
}

bool SerialWebInterface::isWriteBusy() const {
  return static_cast<uint16_t>(_recv_queue.count) + _send_queue.count >= FRAME_QUEUE_SIZE;
}

size_t SerialWebInterface::writeFrame(const uint8_t src[], size_t len) {
  if (!isConnected() || isWriteBusy() || src == nullptr || len < 1 || len > MAX_FRAME_SIZE) return 0;
  return _send_queue.push(src, len) ? len : 0;
}

size_t SerialWebInterface::checkRecvFrame(uint8_t dest[]) {
  if (_restart_at != 0 && static_cast<int32_t>(millis() - _restart_at) >= 0) {
    ESP.restart();
  }
  if (!_enabled) return 0;
  if (_station_active && WiFi.status() == WL_CONNECTED) _current_ip = WiFi.localIP();

  expireHttpSession();
  _http_server.handleClient();
  serviceTcp();
  return _recv_queue.pop(dest);
}

bool SerialWebInterface::saveNetworkConfig(bool station, const String& ssid,
    const String& password) {
  Preferences preferences;
  if (!preferences.begin("rcc6-web", false)) return false;

  // Disable station preference first so a partial NVS write always boots the setup AP.
  bool saved = preferences.putBool("sta-mode", false) == sizeof(bool);
  if (station && saved) {
    saved = preferences.putString("sta-ssid", ssid) == ssid.length();
    if (password.length() == 0) {
      if (preferences.isKey("sta-pass")) saved = preferences.remove("sta-pass") && saved;
    } else {
      saved = preferences.putString("sta-pass", password) == password.length() && saved;
    }
  } else if (!station && saved) {
    if (preferences.isKey("sta-ssid")) saved = preferences.remove("sta-ssid") && saved;
    if (preferences.isKey("sta-pass")) saved = preferences.remove("sta-pass") && saved;
  }
  if (station && saved) saved = preferences.putBool("sta-mode", true) == sizeof(bool);
  preferences.end();

  if (saved) {
    _prefer_station = station;
    if (station) {
      ssid.toCharArray(_station_ssid, sizeof(_station_ssid));
      password.toCharArray(_station_password, sizeof(_station_password));
    } else {
      _station_ssid[0] = '\0';
      _station_password[0] = '\0';
    }
  }
  return saved;
}

void SerialWebInterface::scheduleRestart() {
  _restart_at = millis() + RESTART_DELAY_MS;
  if (_restart_at == 0) _restart_at = 1;
}

bool SerialWebInterface::selectSetupAp() {
  if (!saveNetworkConfig(false, String(), String())) return false;
  scheduleRestart();
  return true;
}

bool SerialWebInterface::clearStoredNetworkConfig() {
  Preferences preferences;
  if (!preferences.begin("rcc6-web", false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  if (cleared) {
    WiFi.disconnect(true, true);
    _prefer_station = false;
    _station_ssid[0] = '\0';
    _station_password[0] = '\0';
  }
  return cleared;
}

void SerialWebInterface::clearSession() {
  if (_tcp_client) _tcp_client.stop();
  _tcp_client = WiFiClient();
  _tcp_connected = false;
  _session = Session::NONE;
  _http_remote = IPAddress();
  _http_session_id[0] = '\0';
  _http_last_activity = 0;
  _http_inflight_seq = 0;
  _recv_queue.clear();
  _send_queue.clear();
  _tcp_tx_length = 0;
  _tcp_tx_offset = 0;
  _http_post_result = HttpPostResult::NONE;
  _http_post_expected = 0;
  _http_post_length = 0;
  resetTcpParser();
}

void SerialWebInterface::resetTcpParser() {
  _tcp_rx_state = TcpRxState::MARKER;
  _tcp_rx_length = 0;
  _tcp_rx_offset = 0;
}

void SerialWebInterface::expireHttpSession() {
  if (_session == Session::HTTP &&
      static_cast<uint32_t>(millis() - _http_last_activity) >= HTTP_SESSION_TIMEOUT_MS &&
      _recv_queue.empty() && _send_queue.empty() && _http_inflight_seq == 0) {
    clearSession();
  }
}

bool SerialWebInterface::httpSessionTokenValid(const String& token) const {
  if (token.length() != 32) return false;
  for (size_t i = 0; i < 32; i++) {
    const char c = token[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool SerialWebInterface::claimHttpSession() {
  expireHttpSession();
  const IPAddress remote = _http_server.client().remoteIP();
  const String token = _http_server.header(HTTP_SESSION_HEADER);

  if (!httpSessionTokenValid(token)) return false;

  if (_session == Session::TCP) {
    if (_tcp_client.connected()) return false;
    clearSession();
  }
  if (_session == Session::HTTP &&
      (_http_remote != remote || strncmp(_http_session_id, token.c_str(), 32) != 0)) {
    if (static_cast<uint32_t>(millis() - _http_last_activity) < HTTP_SESSION_TIMEOUT_MS) {
      return false;
    }
    // The old controller is gone. Preserve outbound/in-flight responses for the
    // new browser to drain, but discard commands the abandoned controller queued.
    _recv_queue.clear();
    _http_post_result = HttpPostResult::NONE;
    _http_post_expected = 0;
    _http_post_length = 0;
    _http_remote = remote;
    token.toCharArray(_http_session_id, sizeof(_http_session_id));
  }
  if (_session == Session::NONE) {
    _session = Session::HTTP;
    _http_remote = remote;
    token.toCharArray(_http_session_id, sizeof(_http_session_id));
  }
  _http_last_activity = millis();
  return true;
}

bool SerialWebInterface::acknowledgeHttpFrame() {
  if (_http_inflight_seq == 0) return false;
  const String value = _http_server.header(HTTP_ACK_HEADER);
  if (value.length() < 1 || value.length() > 10) return false;

  char* end = nullptr;
  const unsigned long ack = strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || ack != _http_inflight_seq) return false;

  _send_queue.pop();
  _http_inflight_seq = 0;
  return true;
}

uint32_t SerialWebInterface::nextHttpSequence() {
  uint32_t sequence = _http_next_seq++;
  if (sequence == 0) sequence = _http_next_seq++;
  return sequence;
}

void SerialWebInterface::rejectTcpClient(WiFiClient& client) {
  client.stop();
}

void SerialWebInterface::serviceTcp() {
  if (_session == Session::TCP && !_tcp_client.connected()) clearSession();

  WiFiClient incoming = _tcp_server.accept();
  if (incoming) {
    if (_session == Session::NONE) {
      _tcp_client = incoming;
      _tcp_client.setNoDelay(true);
      _tcp_connected = true;
      _session = Session::TCP;
      resetTcpParser();
    } else {
      rejectTcpClient(incoming);
    }
  }

  if (_session != Session::TCP) return;

  serviceTcpWrite();
  serviceTcpRead();
}

void SerialWebInterface::serviceTcpRead() {
  size_t budget = TCP_READ_BUDGET;
  while (budget-- > 0 && _tcp_client.available() > 0) {
    if (_tcp_rx_state == TcpRxState::MARKER && isWriteBusy()) return;

    const int value = _tcp_client.read();
    if (value < 0) return;
    const uint8_t byte = static_cast<uint8_t>(value);

    switch (_tcp_rx_state) {
      case TcpRxState::MARKER:
        if (byte == '<') _tcp_rx_state = TcpRxState::LENGTH_LOW;
        break;
      case TcpRxState::LENGTH_LOW:
        _tcp_rx_length = byte;
        _tcp_rx_state = TcpRxState::LENGTH_HIGH;
        break;
      case TcpRxState::LENGTH_HIGH:
        _tcp_rx_length |= static_cast<uint16_t>(byte) << 8;
        if (_tcp_rx_length < 1 || _tcp_rx_length > MAX_FRAME_SIZE) {
          _tcp_client.stop();
          clearSession();
          return;
        }
        _tcp_rx_offset = 0;
        _tcp_rx_state = TcpRxState::PAYLOAD;
        break;
      case TcpRxState::PAYLOAD:
        _tcp_rx_data[_tcp_rx_offset++] = byte;
        if (_tcp_rx_offset == _tcp_rx_length) {
          _recv_queue.push(_tcp_rx_data, _tcp_rx_length);
          resetTcpParser();
        }
        break;
    }
  }
}

void SerialWebInterface::serviceTcpWrite() {
  if (_tcp_tx_length == 0) {
    Frame* frame = _send_queue.front();
    if (frame == nullptr) return;
    _tcp_tx_data[0] = '>';
    _tcp_tx_data[1] = frame->len & 0xff;
    _tcp_tx_data[2] = frame->len >> 8;
    memcpy(&_tcp_tx_data[3], frame->data, frame->len);
    _tcp_tx_length = frame->len + 3;
    _tcp_tx_offset = 0;
  }

  const int fd = _tcp_client.fd();
  if (fd < 0) return;
  const int sent = ::send(fd, _tcp_tx_data + _tcp_tx_offset,
      _tcp_tx_length - _tcp_tx_offset, MSG_DONTWAIT);
  if (sent > 0) {
    _tcp_tx_offset += static_cast<uint16_t>(sent);
    if (_tcp_tx_offset == _tcp_tx_length) {
      _send_queue.pop();
      _tcp_tx_length = 0;
      _tcp_tx_offset = 0;
    }
  } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    _tcp_client.stop();
    clearSession();
  }
}

bool SerialWebInterface::httpAuthorized() {
  if (!_station_active || _http_server.authenticate(HTTP_AUTH_USER, _ap_password)) return true;
  _http_server.requestAuthentication();
  return false;
}

void SerialWebInterface::handleIndex() {
  if (!httpAuthorized()) return;
  _http_server.sendHeader("Content-Encoding", "gzip");
  _http_server.sendHeader("Cache-Control", "no-store");
  _http_server.sendHeader("X-Content-Type-Options", "nosniff");
  _http_server.send_P(200, PSTR("text/html; charset=utf-8"),
      reinterpret_cast<PGM_P>(RCC6_WEB_UI_INDEX_GZ), RCC6_WEB_UI_INDEX_GZ_LEN);
}

void SerialWebInterface::handleHttpGetFrame() {
  if (!httpAuthorized()) return;
  if (!claimHttpSession()) {
    sendHttpStatus(409);
    return;
  }

  acknowledgeHttpFrame();
  Frame* frame = _send_queue.front();
  if (frame == nullptr) {
    sendHttpStatus(204);
    return;
  }

  if (_http_inflight_seq == 0) _http_inflight_seq = nextHttpSequence();
  _http_server.sendHeader("Cache-Control", "no-store");
  _http_server.sendHeader(HTTP_SEQUENCE_HEADER, String(_http_inflight_seq));
  _http_server.setContentLength(frame->len);
  _http_server.send(200, "application/octet-stream", "");
  _http_server.sendContent(reinterpret_cast<const char*>(frame->data), frame->len);
}

void SerialWebInterface::handleHttpPostRaw() {
  HTTPRaw& raw = _http_server.raw();
  switch (raw.status) {
    case RAW_START:
      _http_post_length = 0;
      _http_post_expected = static_cast<uint16_t>(_http_server.clientContentLength());
      _http_server.client().setTimeout(HTTP_BODY_TIMEOUT_MS);
      if (_http_server.clientContentLength() < 1 ||
          _http_server.clientContentLength() > MAX_FRAME_SIZE) {
        _http_post_result = HttpPostResult::INVALID;
        _http_server.client().stop();
      } else if (_station_active && !_http_server.authenticate(HTTP_AUTH_USER, _ap_password)) {
        _http_post_result = HttpPostResult::UNAUTHORIZED;
      } else if (!claimHttpSession()) {
        _http_post_result = HttpPostResult::CONFLICT;
      } else if (isWriteBusy()) {
        _http_post_result = HttpPostResult::FULL;
      } else {
        _http_post_result = HttpPostResult::PENDING;
      }
      break;
    case RAW_WRITE:
      if (_http_post_result == HttpPostResult::PENDING) {
        if (_http_post_length + raw.currentSize > MAX_FRAME_SIZE) {
          _http_post_result = HttpPostResult::INVALID;
        } else {
          memcpy(_http_post_data + _http_post_length, raw.buf, raw.currentSize);
          _http_post_length += raw.currentSize;
        }
      }
      break;
    case RAW_END:
      if (_http_post_result == HttpPostResult::PENDING) {
        if (_http_post_length != _http_post_expected) {
          _http_post_result = HttpPostResult::INVALID;
        } else if (!isAllowedHttpOpcode(_http_post_data[0])) {
          _http_post_result = HttpPostResult::FORBIDDEN;
        } else if (_recv_queue.push(_http_post_data, _http_post_length)) {
          _http_post_result = HttpPostResult::ACCEPTED;
        } else {
          _http_post_result = HttpPostResult::FULL;
        }
      }
      break;
    case RAW_ABORTED:
      _http_post_result = HttpPostResult::ABORTED;
      break;
  }
}

void SerialWebInterface::handleHttpPostFrame() {
  switch (_http_post_result) {
    case HttpPostResult::ACCEPTED:
      sendHttpStatus(204);
      break;
    case HttpPostResult::UNAUTHORIZED:
      _http_server.requestAuthentication();
      break;
    case HttpPostResult::FORBIDDEN:
      sendHttpStatus(403);
      break;
    case HttpPostResult::CONFLICT:
      sendHttpStatus(409);
      break;
    case HttpPostResult::FULL:
      sendHttpStatus(429);
      break;
    case HttpPostResult::INVALID:
    case HttpPostResult::ABORTED:
    case HttpPostResult::NONE:
    case HttpPostResult::PENDING:
      sendHttpStatus(400);
      break;
  }
  _http_post_result = HttpPostResult::NONE;
}

void SerialWebInterface::handleHttpGetNetwork() {
  if (!httpAuthorized()) return;
  if (!claimHttpSession()) {
    sendHttpStatus(409);
    return;
  }

  String response;
  response.reserve(128);
  response += F("{\"mode\":\"");
  response += _station_active ? F("station") : F("ap");
  response += F("\",\"ssid\":\"");
  response += jsonEscape(getCurrentSsid());
  response += F("\",\"ip\":\"");
  response += _current_ip.toString();
  response += F("\",\"fallback\":");
  response += _fallback_active ? F("true") : F("false");
  response += '}';
  _http_server.sendHeader("Cache-Control", "no-store");
  _http_server.send(200, "application/json", response);
}

void SerialWebInterface::handleHttpPostNetwork() {
  if (!httpAuthorized()) return;
  if (!claimHttpSession()) {
    sendHttpStatus(409);
    return;
  }

  const String mode = _http_server.arg("mode");
  if (mode == "ap") {
    if (!saveNetworkConfig(false, String(), String())) {
      sendHttpStatus(500);
      return;
    }
  } else if (mode == "station") {
    const String ssid = _http_server.arg("ssid");
    const String password = _http_server.arg("password");
    if (ssid.length() < 1 || ssid.length() > 32 ||
        (password.length() != 0 && (password.length() < 8 || password.length() > 64))) {
      sendHttpStatus(400);
      return;
    }
    if (!saveNetworkConfig(true, ssid, password)) {
      sendHttpStatus(500);
      return;
    }
  } else {
    sendHttpStatus(400);
    return;
  }

  _http_server.sendHeader("Cache-Control", "no-store");
  _http_server.send(202, "application/json", "{\"ok\":true,\"restart\":true}");
  scheduleRestart();
}

void SerialWebInterface::sendHttpStatus(int status) {
  _http_server.sendHeader("Cache-Control", "no-store");
  _http_server.send(status, "text/plain", "");
}

#endif
