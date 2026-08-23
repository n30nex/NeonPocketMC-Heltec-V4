#ifdef NEONPOCKET_ULTIMATE

#include "UltimateService.h"

#include "DataStore.h"
#include <ultimate_build_sha.h>
#include <esp_heap_caps.h>
#if defined(ESP32) && defined(HELTEC_RCC6)
#include <driver/usb_serial_jtag.h>
#endif
#include <stddef.h>
#include <string.h>

namespace {
constexpr const char* history_path = "/np/history.bin";
constexpr const char* history_new_path = "/np/history.new";
constexpr const char* history_old_path = "/np/history.old";
constexpr const char* meta_a_path = "/np/meta-a.bin";
constexpr const char* meta_b_path = "/np/meta-b.bin";
constexpr const char* settings_path = "/np/settings.bin";
constexpr const char* settings_tmp_path = "/np/settings.tmp";
constexpr const char* composer_path = "/np/composer.bin";
constexpr const char* composer_tmp_path = "/np/composer.tmp";
constexpr const char* metrics_path = "/np/metrics.bin";

bool generationAfter(uint32_t lhs, uint32_t rhs) {
  return static_cast<int32_t>(lhs - rhs) > 0;
}
}

UltimateService ultimate_service;

static constexpr uint8_t displayHopCount(uint8_t encoded_path_len) {
  return encoded_path_len == 0xFF ? 0xFF : (encoded_path_len & 0x3F);
}

static_assert(displayHopCount(0x80) == 0, "3-byte zero-hop path must display as zero hops");
static_assert(displayHopCount(0x81) == 1, "3-byte one-hop path must display as one hop");
static_assert(displayHopCount(0xC2) == 2, "encoded path must display only its hop count");
static_assert(displayHopCount(0xFF) == 0xFF, "direct-route sentinel must remain unknown");

const char* UltimateService::getBuildSha() const {
  return ULTIMATE_BUILD_SHA;
}

uint32_t UltimateService::crc32(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFU;
  while (length--) {
    crc ^= *bytes++;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool UltimateService::validCapacity(uint16_t capacity) {
  return capacity == 0 || capacity == 128 || capacity == 512 || capacity == 2048;
}

void UltimateService::copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  if (source == nullptr) source = "";
  strncpy(destination, source, capacity - 1);
  destination[capacity - 1] = 0;
}

void UltimateService::setDefaults() {
  memset(&settings, 0, sizeof(settings));
  settings.history_capacity = 512;
  settings.scan_cadence_ms = 650;
  settings.private_notifications = false;
  settings.battery_calibration_mv = 0;
  settings.battery_capacity_mah = 0;
  settings.power_profile = static_cast<uint8_t>(UltimatePowerProfile::Balanced);
  static const char* defaults[8] = {
      "On my way", "All good", "Need help", "Please repeat",
      "Received", "Where are you?", "Yes", "No"};
  for (uint8_t i = 0; i < 8; i++) {
    copyText(settings.quick_phrases[i], sizeof(settings.quick_phrases[i]), defaults[i]);
  }
}

bool UltimateService::loadSettings() {
  setDefaults();
  if (!filesystem->exists(settings_path)) {
    if (filesystem->exists(settings_tmp_path)) {
      filesystem->rename(settings_tmp_path, settings_path);
    } else {
      return saveSettings();
    }
  }

  File file = filesystem->open(settings_path, FILE_READ);
  if (!file) return false;
  const size_t stored_size = file.size();
  SettingsFile stored = {};
  bool valid = false;
  if (stored_size == sizeof(stored) &&
      file.read(reinterpret_cast<uint8_t*>(&stored), sizeof(stored)) == sizeof(stored)) {
    if (stored.magic == settings_magic && stored.version == settings_format_version &&
        stored.crc32 == crc32(&stored, offsetof(SettingsFile, crc32))) {
      valid = true;
    } else if (stored.version == format_version) {
      SettingsFileV1 previous;
      memcpy(&previous, &stored, sizeof(previous));
      if (previous.magic == settings_magic &&
          previous.crc32 == crc32(&previous, offsetof(SettingsFileV1, crc32))) {
        stored.magic = settings_magic;
        stored.version = format_version;  // saveSettings() upgrades this v1 file below
        stored.history_capacity = previous.history_capacity;
        stored.scan_cadence_ms = previous.scan_cadence_ms;
        stored.private_notifications = previous.private_notifications;
        memcpy(stored.quick_phrases, previous.quick_phrases, sizeof(stored.quick_phrases));
        valid = true;
      }
    }
  }
  file.close();
  if (!valid || !validCapacity(stored.history_capacity)) {
    Serial.println("Ultimate: settings invalid; using safe defaults");
    return saveSettings();
  }

  settings.history_capacity = stored.history_capacity;
  settings.scan_cadence_ms = stored.scan_cadence_ms;
  if (settings.scan_cadence_ms != 450 && settings.scan_cadence_ms != 650 &&
      settings.scan_cadence_ms != 900 && settings.scan_cadence_ms != 1200) {
    settings.scan_cadence_ms = 650;
  }
  settings.private_notifications = stored.private_notifications != 0;
  settings.battery_calibration_mv = constrain(stored.battery_calibration_mv, -300, 300);
  settings.battery_capacity_mah = stored.battery_capacity_mah == 0 ||
      (stored.battery_capacity_mah >= 50 && stored.battery_capacity_mah <= 20000)
          ? stored.battery_capacity_mah : 0;
  settings.power_profile = stored.power_profile <=
      static_cast<uint8_t>(UltimatePowerProfile::Battery)
          ? stored.power_profile : static_cast<uint8_t>(UltimatePowerProfile::Balanced);
  for (uint8_t i = 0; i < 8; i++) {
    copyText(settings.quick_phrases[i], sizeof(settings.quick_phrases[i]),
             stored.quick_phrases[i]);
  }
  return stored.version == settings_format_version ? true : saveSettings();
}

bool UltimateService::saveSettings() {
  SettingsFile stored = {};
  stored.magic = settings_magic;
  stored.version = settings_format_version;
  stored.history_capacity = settings.history_capacity;
  stored.scan_cadence_ms = settings.scan_cadence_ms;
  stored.private_notifications = settings.private_notifications ? 1 : 0;
  stored.battery_calibration_mv = settings.battery_calibration_mv;
  stored.battery_capacity_mah = settings.battery_capacity_mah;
  stored.power_profile = settings.power_profile;
  for (uint8_t i = 0; i < 8; i++) {
    copyText(stored.quick_phrases[i], sizeof(stored.quick_phrases[i]),
             settings.quick_phrases[i]);
  }
  stored.crc32 = crc32(&stored, offsetof(SettingsFile, crc32));

  if (!removeIfExists(settings_tmp_path)) return false;
  File file = filesystem->open(settings_tmp_path, FILE_WRITE);
  const bool written = file &&
      file.write(reinterpret_cast<const uint8_t*>(&stored), sizeof(stored)) == sizeof(stored);
  if (file) {
    file.flush();
    file.close();
  }
  if (!written) {
    removeIfExists(settings_tmp_path);
    return false;
  }
  if (!removeIfExists(settings_path)) return false;
  return filesystem->rename(settings_tmp_path, settings_path);
}

bool UltimateService::loadComposer() {
  memset(&composer, 0, sizeof(composer));
  if (!filesystem->exists(composer_path)) return saveComposer();
  File file = filesystem->open(composer_path, FILE_READ);
  ComposerFile stored = {};
  const bool complete = file && file.size() == sizeof(stored) &&
      file.read(reinterpret_cast<uint8_t*>(&stored), sizeof(stored)) == sizeof(stored);
  if (file) file.close();
  const bool valid = complete && stored.magic == composer_magic &&
      stored.version == format_version &&
      stored.crc32 == crc32(&stored, offsetof(ComposerFile, crc32)) &&
      stored.state.pinned_kind <= static_cast<uint8_t>(UltimateMessageKind::Channel) &&
      stored.state.draft_kind <= static_cast<uint8_t>(UltimateMessageKind::Channel);
  if (!valid) {
    Serial.println("Ultimate: composer state invalid; starting clean");
    return saveComposer();
  }
  composer = stored.state;
  composer.pinned_label[sizeof(composer.pinned_label) - 1] = 0;
  composer.draft_label[sizeof(composer.draft_label) - 1] = 0;
  composer.draft_text[sizeof(composer.draft_text) - 1] = 0;
  return true;
}

bool UltimateService::saveComposer() {
  ComposerFile stored = {};
  stored.magic = composer_magic;
  stored.version = format_version;
  stored.state = composer;
  stored.crc32 = crc32(&stored, offsetof(ComposerFile, crc32));
  if (!removeIfExists(composer_tmp_path)) return false;
  File file = filesystem->open(composer_tmp_path, FILE_WRITE);
  const bool written = file &&
      file.write(reinterpret_cast<const uint8_t*>(&stored), sizeof(stored)) == sizeof(stored);
  if (file) {
    file.flush();
    file.close();
  }
  if (!written) {
    removeIfExists(composer_tmp_path);
    return false;
  }
  if (!removeIfExists(composer_path)) return false;
  return filesystem->rename(composer_tmp_path, composer_path);
}

bool UltimateService::removeIfExists(const char* path) {
  return !filesystem->exists(path) || filesystem->remove(path);
}

bool UltimateService::loadMeta() {
  auto readCopy = [this](const char* path, JournalMeta& copy) {
    File file = filesystem->open(path, FILE_READ);
    const bool complete = file && file.size() == sizeof(copy) &&
        file.read(reinterpret_cast<uint8_t*>(&copy), sizeof(copy)) == sizeof(copy);
    if (file) file.close();
    return complete && copy.magic == meta_magic && copy.version == format_version &&
        validCapacity(copy.capacity) && copy.capacity != 0 && copy.count <= copy.capacity &&
        copy.head < copy.capacity &&
        copy.crc32 == crc32(&copy, offsetof(JournalMeta, crc32));
  };

  JournalMeta a = {};
  JournalMeta b = {};
  const bool a_valid = readCopy(meta_a_path, a);
  const bool b_valid = readCopy(meta_b_path, b);
  if (a_valid || b_valid) {
    meta = !b_valid || (a_valid && generationAfter(a.generation, b.generation)) ? a : b;
    return true;
  }

  memset(&meta, 0, sizeof(meta));
  meta.magic = meta_magic;
  meta.version = format_version;
  meta.capacity = settings.history_capacity == 0 ? 512 : settings.history_capacity;
  meta.next_sequence = 1;
  return true;
}

bool UltimateService::writeMeta() {
  JournalMeta next = meta;
  next.magic = meta_magic;
  next.version = format_version;
  next.generation = meta.generation + 1;
  next.crc32 = crc32(&next, offsetof(JournalMeta, crc32));
  const char* path = (next.generation & 1U) ? meta_a_path : meta_b_path;
  File file = filesystem->open(path, FILE_WRITE);
  const bool written = file &&
      file.write(reinterpret_cast<const uint8_t*>(&next), sizeof(next)) == sizeof(next);
  if (file) {
    file.flush();
    file.close();
  }
  if (!written) return false;
  meta = next;
  return true;
}

bool UltimateService::recoverJournalSwap() {
  if (!removeIfExists(history_new_path)) return false;
  if (!filesystem->exists(history_old_path)) return true;

  bool current_matches = false;
  if (filesystem->exists(history_path)) {
    File current = filesystem->open(history_path, FILE_READ);
    current_matches = current && current.size() ==
        static_cast<size_t>(meta.capacity) * sizeof(UltimateHistoryRecord);
    if (current) current.close();
  }
  if (current_matches) {
    return removeIfExists(history_old_path);
  }

  if (!removeIfExists(history_path)) return false;
  return filesystem->rename(history_old_path, history_path);
}

bool UltimateService::ensureJournalFile(uint16_t capacity) {
  const size_t expected = static_cast<size_t>(capacity) * sizeof(UltimateHistoryRecord);
  if (filesystem->exists(history_path)) {
    File existing = filesystem->open(history_path, FILE_READ);
    const bool valid = existing && existing.size() == expected;
    if (existing) existing.close();
    return valid;
  }

  File file = filesystem->open(history_path, FILE_WRITE);
  if (!file) return false;
  UltimateHistoryRecord empty = {};
  bool ok = true;
  for (uint16_t slot = 0; slot < capacity && ok; slot++) {
    ok = file.write(reinterpret_cast<const uint8_t*>(&empty), sizeof(empty)) == sizeof(empty);
    if ((slot & 31U) == 31U) yield();
  }
  file.flush();
  file.close();
  return ok;
}

bool UltimateService::readSlot(uint16_t slot, UltimateHistoryRecord& record) const {
  if (slot >= meta.capacity) return false;
  File file = filesystem->open(history_path, FILE_READ);
  if (!file || !file.seek(static_cast<size_t>(slot) * sizeof(record))) {
    if (file) file.close();
    return false;
  }
  const bool complete = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  const bool valid = complete && record.magic == history_magic &&
      record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
  if (valid) record.path_len = displayHopCount(record.path_len);
  return valid;
}

bool UltimateService::writeSlot(uint16_t slot, const UltimateHistoryRecord& record) {
  if (slot >= meta.capacity) return false;
  File file = filesystem->open(history_path, "r+");
  if (!file || !file.seek(static_cast<size_t>(slot) * sizeof(record))) {
    if (file) file.close();
    return false;
  }
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.flush();
  file.close();
  return written;
}

bool UltimateService::appendRecord(const PendingEvent& event) {
  if (settings.history_capacity == 0) return true;
  UltimateHistoryRecord record = {};
  record.magic = history_magic;
  record.sequence = meta.next_sequence;
  record.timestamp = event.timestamp;
  record.flags = event.flags;
  record.kind = event.subtype;
  record.path_len = displayHopCount(event.path_len);
  record.snr_quarter_db = event.snr_quarter_db;
  record.rssi_dbm = event.rssi_dbm;
  record.target = event.target;
  memcpy(record.peer_key, event.peer_key, sizeof(record.peer_key));
  copyText(record.sender, sizeof(record.sender), event.sender);
  copyText(record.text, sizeof(record.text), event.text);
  record.crc32 = crc32(&record, offsetof(UltimateHistoryRecord, crc32));

  UltimateHistoryRecord evicted = {};
  const bool evicted_valid = meta.count == meta.capacity && readSlot(meta.head, evicted);
  if (!writeSlot(meta.head, record)) return false;
  const JournalMeta previous = meta;
  meta.head = (meta.head + 1) % meta.capacity;
  if (meta.count < meta.capacity) meta.count++;
  meta.next_sequence++;
  if (!writeMeta()) {
    meta = previous;
    return false;
  }

  snapshot.last_sequence = record.sequence;
  snapshot.history_count = meta.count;
  if (evicted_valid) removeThreadRecord(evicted);
  if ((record.flags & ULTIMATE_HISTORY_INCOMING) == 0 &&
      delivery.history_sequence == 0 && delivery.kind == record.kind &&
      strncmp(delivery.target, record.sender, sizeof(delivery.target) - 1) == 0 &&
      strncmp(delivery.text, record.text, sizeof(delivery.text) - 1) == 0) {
    delivery.history_sequence = record.sequence;
  }
  if ((record.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
      ULTIMATE_HISTORY_INCOMING) {
    if (snapshot.unread_count < 0xFFFF) snapshot.unread_count++;
  }
  updateThread(record, false);
  return true;
}

bool UltimateService::pushEvent(const PendingEvent& event) {
  bool accepted = false;
  portENTER_CRITICAL(&pending_lock);
  if (pending_count < pending_capacity) {
    pending[pending_head] = event;
    pending_head = (pending_head + 1) % pending_capacity;
    pending_count++;
    accepted = true;
  } else {
    snapshot.event_drops++;
  }
  portEXIT_CRITICAL(&pending_lock);
  return accepted;
}

bool UltimateService::popEvent(PendingEvent& event) {
  bool available = false;
  portENTER_CRITICAL(&pending_lock);
  if (pending_count > 0) {
    event = pending[pending_tail];
    pending_tail = (pending_tail + 1) % pending_capacity;
    pending_count--;
    available = true;
  }
  portEXIT_CRITICAL(&pending_lock);
  return available;
}

bool UltimateService::enqueueMessage(
    UltimateMessageKind kind, bool incoming, uint8_t target, const uint8_t peer_key[6],
    const char* sender, const char* text, uint32_t timestamp, uint8_t path_len,
    int16_t rssi_dbm, int8_t snr_quarter_db) {
  PendingEvent event = {};
  event.type = PendingType::Message;
  event.subtype = static_cast<uint8_t>(kind);
  event.flags = incoming ? ULTIMATE_HISTORY_INCOMING : ULTIMATE_HISTORY_READ;
  event.target = target;
  event.timestamp = timestamp ? timestamp : (clock ? clock->getCurrentTime() : 0);
  event.path_len = displayHopCount(path_len);
  event.rssi_dbm = rssi_dbm;
  event.snr_quarter_db = snr_quarter_db;
  if (peer_key) memcpy(event.peer_key, peer_key, sizeof(event.peer_key));
  if (kind == UltimateMessageKind::Channel && sender && sender[0] != '#') {
    event.sender[0] = '#';
    copyText(event.sender + 1, sizeof(event.sender) - 1, sender);
  } else {
    copyText(event.sender, sizeof(event.sender), sender);
  }
  copyText(event.text, sizeof(event.text), text);
  return pushEvent(event);
}

bool UltimateService::enqueueRadio(
    UltimateRadioEvent radio_event, uint8_t payload_type, int16_t rssi_dbm,
    int8_t snr_quarter_db, uint32_t airtime_ms) {
  PendingEvent event = {};
  event.type = PendingType::Radio;
  event.subtype = static_cast<uint8_t>(radio_event);
  event.target = payload_type;
  event.timestamp = clock ? clock->getCurrentTime() : 0;
  event.airtime_ms = airtime_ms;
  event.rssi_dbm = rssi_dbm;
  event.snr_quarter_db = snr_quarter_db;
  return pushEvent(event);
}

bool UltimateService::enqueueNode(
    const uint8_t peer_key[6], const char* name, uint8_t role, uint8_t path_len,
    uint32_t timestamp, int16_t rssi_dbm, int8_t snr_quarter_db,
    bool signal_attributable) {
  PendingEvent event = {};
  event.type = PendingType::Node;
  event.flags = signal_attributable ? 1 : 0;
  event.role = role;
  event.path_len = displayHopCount(path_len);
  event.timestamp = timestamp;
  event.rssi_dbm = rssi_dbm;
  event.snr_quarter_db = snr_quarter_db;
  if (peer_key) memcpy(event.peer_key, peer_key, sizeof(event.peer_key));
  copyText(event.sender, sizeof(event.sender), name);
  return pushEvent(event);
}

void UltimateService::updateNetwork(const PendingEvent& event) {
  uint8_t use = network_count;
  uint32_t oldest = UINT32_MAX;
  for (uint8_t i = 0; i < network_count; i++) {
    if (memcmp(network[i].key, event.peer_key, sizeof(network[i].key)) == 0) {
      use = i;
      break;
    }
    if (network[i].last_seen < oldest) {
      oldest = network[i].last_seen;
      if (network_count == network_capacity) use = i;
    }
  }
  if (use == network_count && network_count < network_capacity) network_count++;

  UltimateNetworkNode node = use < network_count ? network[use] : UltimateNetworkNode{};
  memcpy(node.key, event.peer_key, sizeof(node.key));
  copyText(node.name, sizeof(node.name), event.sender);
  node.last_seen = event.timestamp;
  node.packet_count++;
  node.role = event.role;
  node.path_len = displayHopCount(event.path_len);
  node.signal_attributable = event.flags != 0;
  node.rssi_dbm = node.signal_attributable ? event.rssi_dbm : 0;
  node.snr_quarter_db = node.signal_attributable ? event.snr_quarter_db : 0;
  if (use > 0) memmove(&network[1], &network[0], use * sizeof(network[0]));
  network[0] = node;
}

void UltimateService::updateThread(const UltimateHistoryRecord& record, bool newest_first) {
  uint8_t found = thread_count;
  for (uint8_t i = 0; i < thread_count; i++) {
    const bool same = threads[i].kind == record.kind &&
        (record.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
             ? threads[i].target == record.target
             : memcmp(threads[i].peer_key, record.peer_key, sizeof(record.peer_key)) == 0);
    if (same) {
      found = i;
      break;
    }
  }

  if (found == thread_count) {
    if (thread_count < thread_capacity) {
      thread_count++;
    } else if (newest_first) {
      return;
    } else {
      found = thread_capacity - 1;
    }
  }

  UltimateThreadSummary summary = found < thread_count ? threads[found] : UltimateThreadSummary{};
  if (summary.newest_sequence == 0 || record.sequence >= summary.newest_sequence) {
    summary.kind = record.kind;
    summary.target = record.target;
    memcpy(summary.peer_key, record.peer_key, sizeof(summary.peer_key));
    copyText(summary.label, sizeof(summary.label), record.sender);
    summary.newest_sequence = record.sequence;
  }
  if ((record.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
      ULTIMATE_HISTORY_INCOMING && summary.unread < 0xFFFF) {
    summary.unread++;
  }

  if (!newest_first && found > 0) {
    memmove(&threads[1], &threads[0], found * sizeof(threads[0]));
    threads[0] = summary;
  } else {
    threads[found] = summary;
  }
}

void UltimateService::removeThreadRecord(const UltimateHistoryRecord& record) {
  const bool unread = (record.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
      ULTIMATE_HISTORY_INCOMING;
  if (unread && snapshot.unread_count) snapshot.unread_count--;

  for (uint8_t i = 0; i < thread_count; i++) {
    const bool same = threads[i].kind == record.kind &&
        (record.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
             ? threads[i].target == record.target
             : memcmp(threads[i].peer_key, record.peer_key, sizeof(record.peer_key)) == 0);
    if (!same) continue;
    if (unread && threads[i].unread) threads[i].unread--;
    if (threads[i].newest_sequence == record.sequence) {
      if (i + 1 < thread_count) {
        memmove(&threads[i], &threads[i + 1], (thread_count - i - 1) * sizeof(threads[0]));
      }
      memset(&threads[thread_count - 1], 0, sizeof(threads[0]));
      thread_count--;
    }
    break;
  }
}

void UltimateService::rebuildThreads() {
  memset(threads, 0, sizeof(threads));
  thread_count = 0;
  snapshot.unread_count = 0;
  if (meta.count == 0 || meta.capacity == 0) return;

  File file = filesystem->open(history_path, FILE_READ);
  if (!file) return;
  for (uint16_t offset = 0; offset < meta.count; offset++) {
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    UltimateHistoryRecord record;
    const bool valid = file.seek(static_cast<size_t>(slot) * sizeof(record)) &&
        file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
        record.magic == history_magic &&
        record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    if (valid) {
      updateThread(record, true);
      if ((record.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
              ULTIMATE_HISTORY_INCOMING && snapshot.unread_count < 0xFFFF) {
        snapshot.unread_count++;
      }
    }
    if ((offset & 31U) == 31U) yield();
  }
  file.close();
}

void UltimateService::updateMetrics(const PendingEvent& event) {
  const uint32_t now = event.timestamp ? event.timestamp : (clock ? clock->getCurrentTime() : 0);
  const uint32_t hour = now / 3600U;
  const uint16_t slot = hour % hourly_capacity;
  UltimateMetricBucket& bucket = hourly[slot];
  if (bucket.hour != hour) {
    memset(&bucket, 0, sizeof(bucket));
    bucket.hour = hour;
  }
  if (event.type == PendingType::Radio) {
    const UltimateRadioEvent kind = static_cast<UltimateRadioEvent>(event.subtype);
    if (kind == UltimateRadioEvent::Rx) {
      bucket.rx_packets++;
      bucket.rssi_total += event.rssi_dbm;
      bucket.snr_quarter_total += event.snr_quarter_db;
      if (bucket.signal_samples < 0xFFFF) bucket.signal_samples++;
    } else if (kind == UltimateRadioEvent::Tx) {
      bucket.tx_packets++;
    } else if (kind == UltimateRadioEvent::TxFailed) {
      bucket.tx_failures++;
    }
    bucket.airtime_ms += event.airtime_ms;
  }
  bucket.battery_mv = snapshot.battery_mv;
  bucket.crc32 = crc32(&bucket, offsetof(UltimateMetricBucket, crc32));
}

void UltimateService::processEvent(const PendingEvent& event) {
  if (event.type == PendingType::Message) {
    if (!appendRecord(event)) snapshot.event_drops++;
  } else if (event.type == PendingType::Node) {
    updateNetwork(event);
  } else if (event.type == PendingType::Radio) {
    const UltimateRadioEvent kind = static_cast<UltimateRadioEvent>(event.subtype);
    if (kind == UltimateRadioEvent::Rx) {
      snapshot.rx_packets++;
      snapshot.last_rssi_dbm = event.rssi_dbm;
      snapshot.last_snr_quarter_db = event.snr_quarter_db;
      snapshot.last_signal_valid = true;
      last_signal_millis = millis();
    } else if (kind == UltimateRadioEvent::Tx) {
      snapshot.tx_packets++;
    } else if (kind == UltimateRadioEvent::TxFailed) {
      snapshot.tx_failures++;
    }
    snapshot.airtime_ms += event.airtime_ms;
  }
  updateMetrics(event);
}

bool UltimateService::saveMetricBucket(uint16_t slot) {
  if (slot >= hourly_capacity) return false;
  File file = filesystem->open(metrics_path, "r+");
  if (!file) return false;
  UltimateMetricBucket bucket = hourly[slot];
  bucket.crc32 = crc32(&bucket, offsetof(UltimateMetricBucket, crc32));
  const bool ok = file.seek(static_cast<size_t>(slot) * sizeof(bucket)) &&
      file.write(reinterpret_cast<const uint8_t*>(&bucket), sizeof(bucket)) == sizeof(bucket);
  file.flush();
  file.close();
  return ok;
}

void UltimateService::loadMetrics() {
  memset(hourly, 0, sizeof(hourly));
  File file = filesystem->open(metrics_path, FILE_READ);
  if (file && file.size() == sizeof(hourly)) {
    file.read(reinterpret_cast<uint8_t*>(hourly), sizeof(hourly));
    file.close();
    for (uint16_t i = 0; i < hourly_capacity; i++) {
      if (hourly[i].crc32 != crc32(&hourly[i], offsetof(UltimateMetricBucket, crc32))) {
        memset(&hourly[i], 0, sizeof(hourly[i]));
      }
    }
    return;
  }
  if (file) file.close();
  file = filesystem->open(metrics_path, FILE_WRITE);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(hourly), sizeof(hourly));
    file.flush();
    file.close();
  }
}

void UltimateService::sampleHighResolution() {
  HighResolutionMetric& sample = high_resolution[high_resolution_head];
  sample.timestamp = clock->getCurrentTime();
  sample.rx_packets = static_cast<uint16_t>(snapshot.rx_packets - last_metric_rx);
  sample.tx_packets = static_cast<uint16_t>(snapshot.tx_packets - last_metric_tx);
  sample.tx_failures = static_cast<uint16_t>(snapshot.tx_failures - last_metric_fail);
  sample.rssi_dbm = snapshot.last_rssi_dbm;
  sample.snr_quarter_db = snapshot.last_snr_quarter_db;
  sample.queue_depth = snapshot.outbound_queue_depth;
  sample.battery_mv = snapshot.battery_mv;
  last_metric_rx = snapshot.rx_packets;
  last_metric_tx = snapshot.tx_packets;
  last_metric_fail = snapshot.tx_failures;
  high_resolution_head = (high_resolution_head + 1) % high_resolution_capacity;
  if (high_resolution_count < high_resolution_capacity) high_resolution_count++;
}

void UltimateService::sampleBatteryProjection() {
  snapshot.battery_projection_valid = false;
  snapshot.battery_trend_mv_per_hour = 0;
  snapshot.battery_runtime_minutes = 0;
  if (snapshot.usb_host_connected || high_resolution_count < 10 || snapshot.battery_mv == 0) return;

  const uint8_t newest_index =
      (high_resolution_head + high_resolution_capacity - 1) % high_resolution_capacity;
  const uint8_t oldest_index =
      (high_resolution_head + high_resolution_capacity - high_resolution_count) %
      high_resolution_capacity;
  const HighResolutionMetric& newest = high_resolution[newest_index];
  const HighResolutionMetric& oldest = high_resolution[oldest_index];
  if (newest.timestamp <= oldest.timestamp) return;
  const uint32_t elapsed = newest.timestamp - oldest.timestamp;
  if (elapsed < 600U) return;

  int32_t trend = (static_cast<int32_t>(newest.battery_mv) - oldest.battery_mv) *
      3600L / static_cast<int32_t>(elapsed);
  trend = constrain(trend, -32768, 32767);
  snapshot.battery_trend_mv_per_hour = static_cast<int16_t>(trend);
  snapshot.battery_projection_valid = true;
  if (trend <= -3 && snapshot.battery_mv > 3450) {
    const uint32_t minutes =
        static_cast<uint32_t>(snapshot.battery_mv - 3450) * 60U /
        static_cast<uint32_t>(-trend);
    snapshot.battery_runtime_minutes =
        static_cast<uint16_t>(minutes > 65535UL ? 65535UL : minutes);
  }
}

void UltimateService::sampleUsbHostState() {
#if defined(ESP32) && (defined(HELTEC_RCC6) || defined(HELTEC_LORA_V3) || defined(HELTEC_LORA_V4))
#if defined(HELTEC_RCC6)
  const bool connected = usb_serial_jtag_is_connected();
#else
  const bool connected = static_cast<bool>(Serial);
#endif
  if (connected) {
    usb_host_seen = true;
    snapshot.battery_projection_valid = false;
    snapshot.battery_trend_mv_per_hour = 0;
    snapshot.battery_runtime_minutes = 0;
  }
  if (!connected && usb_host_connected && usb_host_seen) {
    high_resolution_head = 0;
    high_resolution_count = 0;
    last_metric_rx = snapshot.rx_packets;
    last_metric_tx = snapshot.tx_packets;
    last_metric_fail = snapshot.tx_failures;
    snapshot.battery_projection_valid = false;
    snapshot.battery_trend_mv_per_hour = 0;
    snapshot.battery_runtime_minutes = 0;
    Serial.println("Ultimate: USB host disconnected; battery learning started");
  }
  usb_host_connected = connected;
  snapshot.usb_host_connected = connected;
#else
  snapshot.usb_host_connected = false;
#endif
}

void UltimateService::sampleStatus() {
  snapshot.uptime_seconds = millis() / 1000U;
  snapshot.free_heap = ESP.getFreeHeap();
  snapshot.largest_allocation = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  snapshot.storage_used_kb = store->getStorageUsedKb();
  snapshot.storage_total_kb = store->getStorageTotalKb();
  const uint16_t raw_battery = board->getBattMilliVolts();
  if (raw_battery) {
    const int32_t calibrated = static_cast<int32_t>(raw_battery) +
        settings.battery_calibration_mv;
    snapshot.battery_mv = calibrated > 0 && calibrated <= 4500
        ? static_cast<uint16_t>(calibrated) : 0;
  } else {
    snapshot.battery_mv = 0;
  }
  snapshot.history_count = meta.count;
  snapshot.history_capacity = settings.history_capacity;

  if (snapshot.last_signal_valid && millis() - last_signal_millis > 120000U) {
    snapshot.last_signal_valid = false;
    snapshot.last_rssi_dbm = 0;
    snapshot.last_snr_quarter_db = 0;
  }

  const uint32_t now = clock->getCurrentTime();
  for (uint8_t i = 0; i < network_count; i++) {
    if (network[i].signal_attributable && now - network[i].last_seen > 120) {
      network[i].signal_attributable = false;
      network[i].rssi_dbm = 0;
      network[i].snr_quarter_db = 0;
    }
  }
  sampleHighResolution();
  sampleBatteryProjection();
  snapshot.animation_frame_millis = getRecommendedFrameMillis();
  snapshot.display_timeout_millis = getDisplayTimeoutMillis();
  const uint32_t hour = now / 3600U;
  if (last_hour != 0 && hour != last_hour) {
    saveMetricBucket(last_hour % hourly_capacity);
  }
  last_hour = hour;
}

bool UltimateService::begin(
    fs::FS& fs, mesh::RTCClock& rtc, mesh::MainBoard& main_board, DataStore& data_store) {
  filesystem = &fs;
  clock = &rtc;
  board = &main_board;
  store = &data_store;
  if (!filesystem->exists("/np") && !filesystem->mkdir("/np")) {
    Serial.println("Ultimate: cannot create /np storage namespace");
    return false;
  }
  if (!loadSettings() || !loadComposer() || !loadMeta() || !recoverJournalSwap() ||
      !ensureJournalFile(meta.capacity)) {
    Serial.println("Ultimate: journal initialization failed");
    return false;
  }
  if (meta.generation == 0 && !writeMeta()) return false;
  if (settings.history_capacity != 0 && settings.history_capacity != meta.capacity &&
      !setHistoryCapacity(settings.history_capacity)) {
    return false;
  }

  snapshot.history_count = meta.count;
  snapshot.history_capacity = settings.history_capacity;
  snapshot.last_sequence = meta.next_sequence ? meta.next_sequence - 1 : 0;
  rebuildThreads();
  loadMetrics();
  last_hour = clock->getCurrentTime() / 3600U;
  sampleStatus();
  next_status_sample = millis() + 60000;
  Serial.print("Ultimate: history ready, capacity=");
  Serial.print(settings.history_capacity);
  Serial.print(" records=");
  Serial.println(meta.count);
  Serial.print("Ultimate: build=");
  Serial.println(getBuildSha());
  return true;
}

void UltimateService::loop() {
  PendingEvent event;
  uint8_t budget = 1;  // keep journal writes inside one 66 ms UI frame budget
  while (budget-- && popEvent(event)) processEvent(event);
  const uint32_t now = millis();
  refreshDeliveryState(now);
  if (static_cast<int32_t>(now - next_usb_host_sample) >= 0) {
    next_usb_host_sample = now + 1000;
    sampleUsbHostState();
  }
  if (static_cast<int32_t>(now - next_status_sample) >= 0) {
    next_status_sample = now + 60000;
    sampleStatus();
  }
}

void UltimateService::refreshStatusNow() {
  sampleStatus();
  next_status_sample = millis() + 60000;
}

const UltimateNetworkNode* UltimateService::getNetworkNode(uint8_t index) const {
  return index < network_count ? &network[index] : nullptr;
}

const UltimateThreadSummary* UltimateService::getThread(uint8_t index) const {
  return index < thread_count ? &threads[index] : nullptr;
}

const UltimateMetricBucket* UltimateService::getHourlyMetricNewest(uint16_t offset) const {
  if (offset >= hourly_capacity || clock == nullptr) return nullptr;
  const uint32_t hour = clock->getCurrentTime() / 3600U;
  const UltimateMetricBucket& bucket = hourly[(hour + hourly_capacity - offset) % hourly_capacity];
  return bucket.hour == hour - offset ? &bucket : nullptr;
}

bool UltimateService::getHighResolutionNewest(
    uint8_t offset, uint32_t& timestamp, uint16_t& rx, uint16_t& tx,
    uint16_t& failures, int16_t& rssi, int8_t& snr_quarter,
    uint8_t& queue_depth, uint16_t& battery_mv) const {
  if (offset >= high_resolution_count) return false;
  const uint8_t index = (high_resolution_head + high_resolution_capacity - 1 - offset) %
      high_resolution_capacity;
  const HighResolutionMetric& sample = high_resolution[index];
  timestamp = sample.timestamp;
  rx = sample.rx_packets;
  tx = sample.tx_packets;
  failures = sample.tx_failures;
  rssi = sample.rssi_dbm;
  snr_quarter = sample.snr_quarter_db;
  queue_depth = sample.queue_depth;
  battery_mv = sample.battery_mv;
  return true;
}

bool UltimateService::getHistoryNewest(uint16_t offset, UltimateHistoryRecord& record) const {
  if (offset >= meta.count || meta.capacity == 0) return false;
  const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
  return readSlot(slot, record);
}

bool UltimateService::getNewestUnread(UltimateHistoryRecord& record) const {
  struct Search {
    UltimateHistoryRecord* output;
    bool found;
  } search = {&record, false};
  visitHistory(0, 0, false,
      [](const UltimateHistoryRecord& candidate, void* context) {
        Search* result = static_cast<Search*>(context);
        if ((candidate.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
            ULTIMATE_HISTORY_INCOMING) {
          *result->output = candidate;
          result->found = true;
          return false;
        }
        return true;
      },
      &search);
  return search.found;
}

uint16_t UltimateService::visitHistory(uint32_t before_sequence, uint16_t limit,
    bool oldest_first, UltimateHistoryVisitor visitor, void* context) const {
  if (meta.count == 0 || meta.capacity == 0 || filesystem == nullptr || visitor == nullptr) {
    return 0;
  }
  File file = filesystem->open(history_path, FILE_READ);
  if (!file) return 0;
  uint16_t visited = 0;
  for (uint16_t step = 0; step < meta.count && (limit == 0 || visited < limit); step++) {
    const uint16_t offset = oldest_first ? meta.count - 1 - step : step;
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    UltimateHistoryRecord record;
    const bool valid = file.seek(static_cast<size_t>(slot) * sizeof(record)) &&
        file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
        record.magic == history_magic &&
        record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    if (valid && (before_sequence == 0 || record.sequence < before_sequence)) {
      record.path_len = displayHopCount(record.path_len);
      visited++;
      if (!visitor(record, context)) break;
    }
    if ((step & 31U) == 31U) yield();
  }
  file.close();
  return visited;
}

bool UltimateService::getThreadMessage(uint8_t kind, uint8_t target,
    const uint8_t* peer_key, uint16_t ordinal, UltimateHistoryRecord& record) const {
  if (meta.count == 0 || meta.capacity == 0 || filesystem == nullptr) return false;
  File file = filesystem->open(history_path, "r");
  if (!file) return false;
  uint16_t matched = 0;
  bool found = false;
  for (uint16_t offset = 0; offset < meta.count; offset++) {
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    UltimateHistoryRecord candidate;
    if (!file.seek(static_cast<size_t>(slot) * sizeof(candidate)) ||
        file.read(reinterpret_cast<uint8_t*>(&candidate), sizeof(candidate)) != sizeof(candidate) ||
        candidate.magic != history_magic ||
        candidate.crc32 != crc32(&candidate, offsetof(UltimateHistoryRecord, crc32)) ||
        candidate.kind != kind) {
      continue;
    }
    const bool same = kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
        ? candidate.target == target
        : peer_key != nullptr && memcmp(candidate.peer_key, peer_key, sizeof(candidate.peer_key)) == 0;
    if (same && matched++ == ordinal) {
      candidate.path_len = displayHopCount(candidate.path_len);
      record = candidate;
      found = true;
      break;
    }
    if ((offset & 31U) == 31U) yield();
  }
  file.close();
  return found;
}

bool UltimateService::markRead(uint32_t sequence) {
  File file = filesystem->open(history_path, "r+");
  if (!file) return false;
  UltimateHistoryRecord record;
  for (uint16_t offset = 0; offset < meta.count; offset++) {
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    const size_t position = static_cast<size_t>(slot) * sizeof(record);
    const bool valid = file.seek(position) &&
        file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
        record.magic == history_magic &&
        record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    if (!valid || record.sequence != sequence) {
      if ((offset & 31U) == 31U) yield();
      continue;
    }
    if ((record.flags & ULTIMATE_HISTORY_READ) != 0) {
      file.close();
      return true;
    }
    record.flags |= ULTIMATE_HISTORY_READ;
    record.path_len = displayHopCount(record.path_len);
    record.crc32 = crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    const bool written = file.seek(position) &&
        file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    file.flush();
    file.close();
    if (!written) return false;
    if (snapshot.unread_count) snapshot.unread_count--;
    for (uint8_t i = 0; i < thread_count; i++) {
      const bool same = threads[i].kind == record.kind &&
          (record.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
               ? threads[i].target == record.target
               : memcmp(threads[i].peer_key, record.peer_key, sizeof(record.peer_key)) == 0);
      if (same && threads[i].unread) {
        threads[i].unread--;
        break;
      }
    }
    return true;
  }
  file.close();
  return false;
}

bool UltimateService::markAllRead() {
  if (snapshot.unread_count == 0) return true;
  File file = filesystem->open(history_path, "r+");
  if (!file) return false;
  bool success = true;
  UltimateHistoryRecord record;
  for (uint16_t offset = 0; offset < meta.count; offset++) {
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    const size_t position = static_cast<size_t>(slot) * sizeof(record);
    const bool valid = file.seek(position) &&
        file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
        record.magic == history_magic &&
        record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    if (!valid) {
      success = false;
    } else if ((record.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
               ULTIMATE_HISTORY_INCOMING) {
      record.flags |= ULTIMATE_HISTORY_READ;
      record.crc32 = crc32(&record, offsetof(UltimateHistoryRecord, crc32));
      success = file.seek(position) &&
          file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
          success;
    }
    if ((offset & 31U) == 31U) yield();
  }
  file.flush();
  file.close();
  rebuildThreads();
  return success;
}

bool UltimateService::clearHistory() {
  if (!removeIfExists(history_path)) return false;
  if (!ensureJournalFile(meta.capacity)) return false;
  meta.count = 0;
  meta.head = 0;
  if (!writeMeta()) return false;
  snapshot.history_count = 0;
  snapshot.unread_count = 0;
  memset(threads, 0, sizeof(threads));
  thread_count = 0;
  return true;
}

bool UltimateService::setHistoryCapacity(uint16_t capacity) {
  if (!validCapacity(capacity)) return false;
  if (capacity == 0) {
    settings.history_capacity = 0;
    snapshot.history_capacity = 0;
    return saveSettings();
  }
  if (capacity == meta.capacity) {
    settings.history_capacity = capacity;
    snapshot.history_capacity = capacity;
    return saveSettings();
  }

  const uint16_t retain = meta.count < capacity ? meta.count : capacity;
  if (!removeIfExists(history_new_path)) return false;
  File next = filesystem->open(history_new_path, FILE_WRITE);
  if (!next) return false;
  File current = filesystem->open(history_path, FILE_READ);
  if (!current) {
    next.close();
    removeIfExists(history_new_path);
    return false;
  }
  UltimateHistoryRecord empty = {};
  bool ok = true;
  for (uint16_t slot = 0; slot < capacity && ok; slot++) {
    ok = next.write(reinterpret_cast<const uint8_t*>(&empty), sizeof(empty)) == sizeof(empty);
  }
  UltimateHistoryRecord record;
  for (uint16_t i = 0; i < retain && ok; i++) {
    const uint16_t offset = retain - 1 - i;
    const uint16_t slot = (meta.head + meta.capacity - 1 - offset) % meta.capacity;
    const bool valid = current.seek(static_cast<size_t>(slot) * sizeof(record)) &&
        current.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
        record.magic == history_magic &&
        record.crc32 == crc32(&record, offsetof(UltimateHistoryRecord, crc32));
    if (!valid ||
        !next.seek(static_cast<size_t>(i) * sizeof(record))) {
      ok = false;
      break;
    }
    ok = next.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    if ((i & 31U) == 31U) yield();
  }
  current.close();
  next.flush();
  next.close();
  if (!ok) {
    removeIfExists(history_new_path);
    return false;
  }

  if (!removeIfExists(history_old_path)) {
    removeIfExists(history_new_path);
    return false;
  }
  if (!filesystem->rename(history_path, history_old_path) ||
      !filesystem->rename(history_new_path, history_path)) {
    removeIfExists(history_path);
    filesystem->rename(history_old_path, history_path);
    removeIfExists(history_new_path);
    return false;
  }

  const JournalMeta old_meta = meta;
  meta.capacity = capacity;
  meta.count = retain;
  meta.head = retain % capacity;
  if (!writeMeta()) {
    meta = old_meta;
    removeIfExists(history_path);
    filesystem->rename(history_old_path, history_path);
    return false;
  }
  removeIfExists(history_old_path);
  settings.history_capacity = capacity;
  snapshot.history_capacity = capacity;
  snapshot.history_count = retain;
  rebuildThreads();
  return saveSettings();
}

bool UltimateService::updateSettings(const UltimateSettings& updated) {
  if (!validCapacity(updated.history_capacity) ||
      (updated.scan_cadence_ms != 450 && updated.scan_cadence_ms != 650 &&
       updated.scan_cadence_ms != 900 && updated.scan_cadence_ms != 1200) ||
      updated.battery_calibration_mv < -300 || updated.battery_calibration_mv > 300 ||
      (updated.battery_capacity_mah != 0 &&
       (updated.battery_capacity_mah < 50 || updated.battery_capacity_mah > 20000)) ||
      updated.power_profile > static_cast<uint8_t>(UltimatePowerProfile::Battery)) {
    return false;
  }
  UltimateSettings safe = updated;
  for (uint8_t i = 0; i < 8; i++) {
    safe.quick_phrases[i][sizeof(safe.quick_phrases[i]) - 1] = 0;
  }
  if (safe.history_capacity != settings.history_capacity &&
      !setHistoryCapacity(safe.history_capacity)) {
    return false;
  }
  settings = safe;
  snapshot.history_capacity = settings.history_capacity;
  snapshot.animation_frame_millis = getRecommendedFrameMillis();
  snapshot.display_timeout_millis = getDisplayTimeoutMillis();
  return saveSettings();
}

bool UltimateService::setPinnedTarget(uint8_t kind, uint8_t target,
    const uint8_t* peer_key, const char* label) {
  if (kind < static_cast<uint8_t>(UltimateMessageKind::Direct) ||
      kind > static_cast<uint8_t>(UltimateMessageKind::Channel) ||
      (kind == static_cast<uint8_t>(UltimateMessageKind::Direct) && peer_key == nullptr)) {
    return false;
  }
  composer.pinned_kind = kind;
  composer.pinned_target = target;
  memset(composer.pinned_key, 0, sizeof(composer.pinned_key));
  if (peer_key) memcpy(composer.pinned_key, peer_key, sizeof(composer.pinned_key));
  copyText(composer.pinned_label, sizeof(composer.pinned_label), label);
  return saveComposer();
}

bool UltimateService::clearPinnedTarget() {
  composer.pinned_kind = 0;
  composer.pinned_target = 0;
  memset(composer.pinned_key, 0, sizeof(composer.pinned_key));
  composer.pinned_label[0] = 0;
  return saveComposer();
}

bool UltimateService::saveDraft(uint8_t kind, uint8_t target,
    const uint8_t* peer_key, const char* label, const char* text) {
  if (text == nullptr || text[0] == 0) return clearDraft();
  if (kind < static_cast<uint8_t>(UltimateMessageKind::Direct) ||
      kind > static_cast<uint8_t>(UltimateMessageKind::Channel) ||
      (kind == static_cast<uint8_t>(UltimateMessageKind::Direct) && peer_key == nullptr)) {
    return false;
  }
  composer.draft_kind = kind;
  composer.draft_target = target;
  memset(composer.draft_key, 0, sizeof(composer.draft_key));
  if (peer_key) memcpy(composer.draft_key, peer_key, sizeof(composer.draft_key));
  copyText(composer.draft_label, sizeof(composer.draft_label), label);
  copyText(composer.draft_text, sizeof(composer.draft_text), text);
  return saveComposer();
}

bool UltimateService::clearDraft() {
  composer.draft_kind = 0;
  composer.draft_target = 0;
  memset(composer.draft_key, 0, sizeof(composer.draft_key));
  composer.draft_label[0] = 0;
  composer.draft_text[0] = 0;
  return saveComposer();
}

void UltimateService::startDelivery(uint8_t kind, const char* target,
    const char* text, uint32_t expected_ack, uint32_t timeout_millis) {
  delivery = {};
  delivery.state = UltimateDeliveryState::Queued;
  delivery.kind = kind;
  delivery.ack_expected = expected_ack != 0;
  delivery.expected_ack = expected_ack;
  delivery.started_millis = delivery.changed_millis = millis();
  const uint32_t ack_timeout = timeout_millis < 5000UL ? 5000UL : timeout_millis;
  delivery.deadline_millis = delivery.started_millis +
      (delivery.ack_expected ? ack_timeout + 2000UL : 30000UL);
  copyText(delivery.target, sizeof(delivery.target), target);
  copyText(delivery.text, sizeof(delivery.text), text);
}

void UltimateService::markDeliveryOnAir() {
  if (delivery.state != UltimateDeliveryState::Queued) return;
  delivery.state = delivery.ack_expected
      ? UltimateDeliveryState::OnAir : UltimateDeliveryState::Transmitted;
  delivery.changed_millis = millis();
}

void UltimateService::markDeliveryFailed() {
  if (delivery.state == UltimateDeliveryState::Idle ||
      delivery.state == UltimateDeliveryState::Acked) return;
  delivery.state = UltimateDeliveryState::Failed;
  delivery.changed_millis = millis();
}

void UltimateService::markDeliveryAcked(uint32_t expected_ack,
    uint32_t round_trip_millis) {
  if (!delivery.ack_expected || expected_ack == 0 ||
      delivery.expected_ack != expected_ack) return;
  delivery.state = UltimateDeliveryState::Acked;
  delivery.round_trip_millis = round_trip_millis;
  delivery.changed_millis = millis();
}

void UltimateService::refreshDeliveryState(uint32_t now) {
  if (delivery.deadline_millis == 0 ||
      (delivery.state != UltimateDeliveryState::Queued &&
       delivery.state != UltimateDeliveryState::OnAir)) return;
  if (static_cast<int32_t>(now - delivery.deadline_millis) >= 0) {
    delivery.state = delivery.ack_expected
        ? UltimateDeliveryState::NoAck : UltimateDeliveryState::Unconfirmed;
    delivery.changed_millis = now;
  }
}

uint16_t UltimateService::getRecommendedFrameMillis() const {
  uint16_t interval = settings.power_profile ==
      static_cast<uint8_t>(UltimatePowerProfile::Battery) ? 125 : 66;
  const uint32_t flush = snapshot.display_flush_ema_micros;
  if (snapshot.outbound_queue_depth >= 12 || flush >= 90000 ||
      (snapshot.largest_allocation && snapshot.largest_allocation < 32768)) {
    if (interval < 150) interval = 150;
  } else if (snapshot.outbound_queue_depth >= 6 || flush >= 45000) {
    if (interval < 100) interval = 100;
  }
  return interval;
}

uint32_t UltimateService::getDisplayTimeoutMillis() const {
  switch (static_cast<UltimatePowerProfile>(settings.power_profile)) {
    case UltimatePowerProfile::Field: return 300000U;
    case UltimatePowerProfile::Battery: return 30000U;
    default: return 60000U;
  }
}

void UltimateService::setDisplayTransfer(uint32_t micros, uint16_t tiles) {
  snapshot.display_flush_micros = micros;
  snapshot.display_tiles_sent = tiles;
  snapshot.display_flush_ema_micros = snapshot.display_flush_ema_micros
      ? (snapshot.display_flush_ema_micros * 3U + micros) / 4U : micros;
  snapshot.animation_frame_millis = getRecommendedFrameMillis();
}

#endif
