#pragma once

#include "../BaseSerialInterface.h"
#include <WebServer.h>
#include <WiFi.h>

class SerialWebInterface : public BaseSerialInterface {
public:
  SerialWebInterface();

  void begin(const char* node_name, uint16_t tcp_port = 5000);

  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _enabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;

  const char* getApSsid() const { return _ap_ssid; }
  const char* getApPassword() const { return _ap_password; }
  IPAddress getApIP() const;
  bool isStationMode() const { return _station_active; }
  const char* getCurrentSsid() const;
  IPAddress getCurrentIP() const { return _current_ip; }
  bool isFallbackActive() const { return _fallback_active; }
  bool selectSetupAp();
  bool clearStoredNetworkConfig();
#ifdef NEONPOCKET_ULTIMATE
  WebServer& httpServer() { return _http_server; }
  bool authorizeHttpRequest() { return httpAuthorized(); }
#endif

private:
  static constexpr uint8_t FRAME_QUEUE_SIZE = 8;
  static constexpr uint32_t HTTP_SESSION_TIMEOUT_MS = 10000;
  static constexpr uint32_t HTTP_BODY_TIMEOUT_MS = 250;
  static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
  static constexpr uint32_t RESTART_DELAY_MS = 750;
  static constexpr size_t TCP_READ_BUDGET = 384;

  struct Frame {
    uint16_t len;
    uint8_t data[MAX_FRAME_SIZE];
  };

  struct FrameQueue {
    Frame frames[FRAME_QUEUE_SIZE];
    uint8_t head;
    uint8_t count;

    void clear();
    bool full() const;
    bool empty() const;
    bool push(const uint8_t* data, size_t len);
    Frame* front();
    void pop();
    size_t pop(uint8_t* dest);
  };

  enum class Session : uint8_t { NONE, TCP, HTTP };
  enum class TcpRxState : uint8_t { MARKER, LENGTH_LOW, LENGTH_HIGH, PAYLOAD };
  enum class HttpPostResult : uint8_t {
    NONE, PENDING, ACCEPTED, INVALID, UNAUTHORIZED, FORBIDDEN, CONFLICT, FULL, ABORTED
  };

  bool _initialized;
  bool _enabled;
  bool _routes_registered;
  bool _tcp_connected;
  uint16_t _tcp_port;
  char _ap_ssid[33];
  char _ap_password[9];
  bool _prefer_station;
  bool _station_active;
  bool _fallback_active;
  char _station_ssid[33];
  char _station_password[65];
  IPAddress _current_ip;
  uint32_t _restart_at;

  WiFiServer _tcp_server;
  WiFiClient _tcp_client;
  WebServer _http_server;
  Session _session;
  IPAddress _http_remote;
  char _http_session_id[33];
  uint32_t _http_last_activity;
  uint32_t _http_inflight_seq;
  uint32_t _http_next_seq;

  FrameQueue _recv_queue;
  FrameQueue _send_queue;

  TcpRxState _tcp_rx_state;
  uint16_t _tcp_rx_length;
  uint16_t _tcp_rx_offset;
  uint8_t _tcp_rx_data[MAX_FRAME_SIZE];
  uint8_t _tcp_tx_data[MAX_FRAME_SIZE + 3];
  uint16_t _tcp_tx_length;
  uint16_t _tcp_tx_offset;

  HttpPostResult _http_post_result;
  uint16_t _http_post_expected;
  uint16_t _http_post_length;
  uint8_t _http_post_data[MAX_FRAME_SIZE];

  void configureIdentity(const char* node_name);
  void registerHttpRoutes();
  bool startAccessPoint();
  void startServers();
  bool httpAuthorized();
  bool saveNetworkConfig(bool station, const String& ssid, const String& password);
  void scheduleRestart();
  void clearSession();
  void resetTcpParser();
  void expireHttpSession();
  bool claimHttpSession();
  bool httpSessionTokenValid(const String& token) const;
  bool acknowledgeHttpFrame();
  uint32_t nextHttpSequence();
  void serviceTcp();
  void serviceTcpRead();
  void serviceTcpWrite();
  void rejectTcpClient(WiFiClient& client);

  void handleIndex();
  void handleHttpGetFrame();
  void handleHttpPostFrame();
  void handleHttpPostRaw();
  void handleHttpGetNetwork();
  void handleHttpPostNetwork();
  void sendHttpStatus(int status);
};
