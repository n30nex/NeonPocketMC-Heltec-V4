#pragma once

#ifdef NEONPOCKET_ULTIMATE

#include <Arduino.h>
#include <FS.h>
#include <MeshCore.h>

class DataStore;

enum class UltimateMessageKind : uint8_t {
  Direct = 1,
  Channel = 2,
};

enum class UltimateRadioEvent : uint8_t {
  Rx = 1,
  Tx = 2,
  TxFailed = 3,
};

enum class UltimateDeliveryState : uint8_t {
  Idle = 0,
  Queued,
  OnAir,
  Transmitted,
  Acked,
  NoAck,
  Unconfirmed,
  Failed,
};

enum class UltimatePowerProfile : uint8_t {
  Balanced = 0,
  Field = 1,
  Battery = 2,
};

enum UltimateHistoryFlags : uint8_t {
  ULTIMATE_HISTORY_INCOMING = 0x01,
  ULTIMATE_HISTORY_READ = 0x02,
};

#pragma pack(push, 1)
struct UltimateHistoryRecord {
  uint32_t magic;
  uint32_t sequence;
  uint32_t timestamp;
  uint8_t flags;
  uint8_t kind;
  uint8_t path_len;
  int8_t snr_quarter_db;
  int16_t rssi_dbm;
  uint8_t target;
  uint8_t reserved;
  uint8_t peer_key[6];
  char sender[32];
  char text[194];
  uint32_t crc32;
};

struct UltimateMetricBucket {
  uint32_t hour;
  uint32_t rx_packets;
  uint32_t tx_packets;
  uint32_t tx_failures;
  uint32_t airtime_ms;
  int32_t rssi_total;
  int32_t snr_quarter_total;
  uint16_t signal_samples;
  uint16_t battery_mv;
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(UltimateHistoryRecord) == 256,
              "Ultimate journal records must remain exactly 256 bytes");

using UltimateHistoryVisitor = bool (*)(const UltimateHistoryRecord& record, void* context);

struct UltimateNetworkNode {
  uint8_t key[6];
  char name[28];
  uint32_t last_seen;
  uint32_t packet_count;
  int16_t rssi_dbm;
  int8_t snr_quarter_db;
  uint8_t role;
  uint8_t path_len;
  bool signal_attributable;
};

struct UltimateThreadSummary {
  uint8_t kind;
  uint8_t target;
  uint8_t peer_key[6];
  char label[32];
  uint32_t newest_sequence;
  uint16_t unread;
};

struct UltimateDeliverySnapshot {
  UltimateDeliveryState state = UltimateDeliveryState::Idle;
  uint8_t kind = 0;
  bool ack_expected = false;
  uint32_t expected_ack = 0;
  uint32_t history_sequence = 0;
  uint32_t started_millis = 0;
  uint32_t changed_millis = 0;
  uint32_t deadline_millis = 0;
  uint32_t round_trip_millis = 0;
  char target[32] = {};
  char text[48] = {};
};

struct UltimateComposerState {
  uint8_t pinned_kind = 0;
  uint8_t pinned_target = 0;
  uint8_t pinned_key[6] = {};
  char pinned_label[32] = {};
  uint8_t draft_kind = 0;
  uint8_t draft_target = 0;
  uint8_t draft_key[6] = {};
  char draft_label[32] = {};
  char draft_text[141] = {};
};

struct UltimateSnapshot {
  uint32_t uptime_seconds;
  uint32_t rx_packets;
  uint32_t tx_packets;
  uint32_t tx_failures;
  uint32_t airtime_ms;
  uint32_t event_drops;
  uint32_t free_heap;
  uint32_t largest_allocation;
  uint32_t storage_used_kb;
  uint32_t storage_total_kb;
  uint32_t last_sequence;
  uint16_t battery_mv;
  uint16_t history_count;
  uint16_t history_capacity;
  uint16_t unread_count;
  uint16_t display_tiles_sent;
  uint32_t display_flush_micros;
  uint32_t display_flush_ema_micros;
  uint32_t memory_gate_last_pass_seconds;
  int16_t battery_trend_mv_per_hour;
  uint16_t battery_runtime_minutes;
  uint16_t animation_frame_millis;
  uint32_t display_timeout_millis;
  int16_t last_rssi_dbm;
  int8_t last_snr_quarter_db;
  uint8_t outbound_queue_depth;
  bool last_signal_valid;
  bool memory_gate_passed;
  bool battery_projection_valid;
  bool usb_host_connected;
};

struct UltimateSettings {
  uint16_t history_capacity;
  uint16_t scan_cadence_ms;
  bool private_notifications;
  int16_t battery_calibration_mv;
  uint16_t battery_capacity_mah;
  uint8_t power_profile;
  char quick_phrases[8][48];
};

class UltimateService {
  enum class PendingType : uint8_t { Message, Radio, Node };

  struct PendingEvent {
    PendingType type;
    uint8_t subtype;
    uint8_t flags;
    uint8_t path_len;
    uint8_t target;
    uint8_t peer_key[6];
    uint32_t timestamp;
    uint32_t airtime_ms;
    int16_t rssi_dbm;
    int8_t snr_quarter_db;
    uint8_t role;
    char sender[32];
    char text[194];
  };

#pragma pack(push, 1)
  struct JournalMeta {
    uint32_t magic;
    uint32_t generation;
    uint32_t next_sequence;
    uint16_t version;
    uint16_t capacity;
    uint16_t count;
    uint16_t head;
    uint32_t reserved[2];
    uint32_t crc32;
  };

  struct SettingsFile {
    uint32_t magic;
    uint16_t version;
    uint16_t history_capacity;
    uint16_t scan_cadence_ms;
    uint8_t private_notifications;
    int16_t battery_calibration_mv;
    uint8_t power_profile;
    uint16_t battery_capacity_mah;
    char quick_phrases[8][48];
    uint32_t crc32;
  };

  struct SettingsFileV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t history_capacity;
    uint16_t scan_cadence_ms;
    uint8_t private_notifications;
    uint8_t reserved[5];
    char quick_phrases[8][48];
    uint32_t crc32;
  };
  static_assert(sizeof(SettingsFile) == sizeof(SettingsFileV1),
                "Ultimate settings migration must remain in-place");

  struct ComposerFile {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    UltimateComposerState state;
    uint32_t crc32;
  };

  struct HighResolutionMetric {
    uint32_t timestamp;
    uint16_t rx_packets;
    uint16_t tx_packets;
    uint16_t tx_failures;
    int16_t rssi_dbm;
    int8_t snr_quarter_db;
    uint8_t queue_depth;
    uint16_t battery_mv;
  };
#pragma pack(pop)

  static constexpr uint32_t history_magic = 0x32504E48;  // HNP2
  static constexpr uint32_t meta_magic = 0x32504E4D;     // MNP2
  static constexpr uint32_t settings_magic = 0x32504E53; // SNP2
  static constexpr uint32_t composer_magic = 0x32504E43; // CNP2
  static constexpr uint16_t format_version = 1;
  static constexpr uint16_t settings_format_version = 2;
  static constexpr uint8_t pending_capacity = 32;
  static constexpr uint8_t network_capacity = 64;
  static constexpr uint8_t thread_capacity = 32;
  static constexpr uint16_t hourly_capacity = 168;
  static constexpr uint8_t high_resolution_capacity = 120;

  fs::FS* filesystem = nullptr;
  mesh::RTCClock* clock = nullptr;
  mesh::MainBoard* board = nullptr;
  DataStore* store = nullptr;

  PendingEvent pending[pending_capacity] = {};
  volatile uint8_t pending_head = 0;
  volatile uint8_t pending_tail = 0;
  volatile uint8_t pending_count = 0;
  portMUX_TYPE pending_lock = portMUX_INITIALIZER_UNLOCKED;

  UltimateNetworkNode network[network_capacity] = {};
  uint8_t network_count = 0;
  UltimateThreadSummary threads[thread_capacity] = {};
  uint8_t thread_count = 0;
  UltimateMetricBucket hourly[hourly_capacity] = {};
  HighResolutionMetric high_resolution[high_resolution_capacity] = {};
  uint8_t high_resolution_head = 0;
  uint8_t high_resolution_count = 0;

  JournalMeta meta = {};
  UltimateSettings settings = {};
  UltimateComposerState composer = {};
  UltimateDeliverySnapshot delivery = {};
  UltimateSnapshot snapshot = {};
  uint32_t last_hour = 0;
  uint32_t next_status_sample = 0;
  uint32_t last_metric_rx = 0;
  uint32_t last_metric_tx = 0;
  uint32_t last_metric_fail = 0;
  uint32_t last_signal_millis = 0;
  uint32_t next_usb_host_sample = 0;
  bool usb_host_seen = false;
  bool usb_host_connected = false;

  static uint32_t crc32(const void* data, size_t length);
  static bool validCapacity(uint16_t capacity);
  static void copyText(char* destination, size_t capacity, const char* source);
  void setDefaults();
  bool loadSettings();
  bool saveSettings();
  bool loadComposer();
  bool saveComposer();
  bool loadMeta();
  bool writeMeta();
  bool removeIfExists(const char* path);
  bool ensureJournalFile(uint16_t capacity);
  bool recoverJournalSwap();
  bool readSlot(uint16_t slot, UltimateHistoryRecord& record) const;
  bool writeSlot(uint16_t slot, const UltimateHistoryRecord& record);
  bool appendRecord(const PendingEvent& event);
  bool popEvent(PendingEvent& event);
  bool pushEvent(const PendingEvent& event);
  void processEvent(const PendingEvent& event);
  void updateNetwork(const PendingEvent& event);
  void updateThread(const UltimateHistoryRecord& record, bool newest_first);
  void rebuildThreads();
  void updateMetrics(const PendingEvent& event);
  void sampleStatus();
  void sampleHighResolution();
  void sampleBatteryProjection();
  void sampleUsbHostState();
  void refreshDeliveryState(uint32_t now);
  bool saveMetricBucket(uint16_t slot);
  void loadMetrics();

public:
  bool begin(fs::FS& fs, mesh::RTCClock& rtc, mesh::MainBoard& main_board, DataStore& data_store);
  void loop();
  void refreshStatusNow();

  bool enqueueMessage(UltimateMessageKind kind, bool incoming, uint8_t target,
                      const uint8_t peer_key[6], const char* sender, const char* text,
                      uint32_t timestamp, uint8_t path_len, int16_t rssi_dbm,
                      int8_t snr_quarter_db);
  bool enqueueRadio(UltimateRadioEvent event, uint8_t payload_type,
                    int16_t rssi_dbm = 0, int8_t snr_quarter_db = 0,
                    uint32_t airtime_ms = 0);
  bool enqueueNode(const uint8_t peer_key[6], const char* name, uint8_t role,
                   uint8_t path_len, uint32_t timestamp, int16_t rssi_dbm,
                   int8_t snr_quarter_db, bool signal_attributable);

  const UltimateSnapshot& getSnapshot() const { return snapshot; }
  const char* getBuildSha() const;
  const UltimateSettings& getSettings() const { return settings; }
  const UltimateComposerState& getComposerState() const { return composer; }
  const UltimateDeliverySnapshot& getDelivery() const { return delivery; }
  uint8_t getNetworkCount() const { return network_count; }
  const UltimateNetworkNode* getNetworkNode(uint8_t index) const;
  uint8_t getThreadCount() const { return thread_count; }
  const UltimateThreadSummary* getThread(uint8_t index) const;
  const UltimateMetricBucket* getHourlyMetricNewest(uint16_t offset) const;
  bool getHighResolutionNewest(uint8_t offset, uint32_t& timestamp, uint16_t& rx,
                               uint16_t& tx, uint16_t& failures, int16_t& rssi,
                               int8_t& snr_quarter, uint8_t& queue_depth,
                               uint16_t& battery_mv) const;
  uint16_t getHistoryCount() const { return meta.count; }
  bool getHistoryNewest(uint16_t offset, UltimateHistoryRecord& record) const;
  bool getNewestUnread(UltimateHistoryRecord& record) const;
  uint16_t visitHistory(uint32_t before_sequence, uint16_t limit, bool oldest_first,
                        UltimateHistoryVisitor visitor, void* context) const;
  bool getThreadMessage(uint8_t kind, uint8_t target, const uint8_t* peer_key,
                        uint16_t ordinal, UltimateHistoryRecord& record) const;
  uint32_t getNewestSequence() const { return meta.count ? meta.next_sequence - 1 : 0; }
  bool markRead(uint32_t sequence);
  bool markAllRead();
  bool clearHistory();
  bool setHistoryCapacity(uint16_t capacity);
  bool updateSettings(const UltimateSettings& updated);
  bool setPinnedTarget(uint8_t kind, uint8_t target, const uint8_t* peer_key,
                       const char* label);
  bool clearPinnedTarget();
  bool saveDraft(uint8_t kind, uint8_t target, const uint8_t* peer_key,
                 const char* label, const char* text);
  bool clearDraft();
  void startDelivery(uint8_t kind, const char* target, const char* text,
                     uint32_t expected_ack, uint32_t timeout_millis);
  void markDeliveryOnAir();
  void markDeliveryFailed();
  void markDeliveryAcked(uint32_t expected_ack, uint32_t round_trip_millis);
  uint16_t getRecommendedFrameMillis() const;
  uint32_t getDisplayTimeoutMillis() const;
  void setMemoryGatePassed(bool passed) {
    snapshot.memory_gate_passed = passed;
    if (passed) snapshot.memory_gate_last_pass_seconds = millis() / 1000U;
  }
  void setQueueDepth(uint8_t depth) { snapshot.outbound_queue_depth = depth; }
  void setDisplayTransfer(uint32_t micros, uint16_t tiles);
};

extern UltimateService ultimate_service;

#endif
