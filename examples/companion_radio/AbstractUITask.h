#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "NodePrefs.h"

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

enum class UIRadioEvent : uint8_t {
    Rx,
    TxComplete,
    TxFailed
};

class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  MultiSerialInterface* _interfaceManager;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, MultiSerialInterface* interfaceManager) : _board(board), _interfaceManager(interfaceManager) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isSerialEnabled() const { return _interfaceManager->isEnabled(); }
  void enableSerial() { _interfaceManager->enable(); }
  void disableSerial() { _interfaceManager->disable(); }
  bool isBluetoothEnabled() const { return _interfaceManager->isBluetoothEnabled(); }
  void enableBluetooth() { _interfaceManager->enableBluetooth(); }
  void disableBluetooth() { _interfaceManager->disableBluetooth(); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) = 0;
  virtual void newMsgWithEvent(uint8_t path_len, const char* from_name, const char* text,
                               int msgcount, UIEventType type) {
    (void) type;
    newMsg(path_len, from_name, text, msgcount);
  }
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void onNewContactVisual() { }
  virtual void onRadioEvent(UIRadioEvent event, uint8_t payload_type,
                            int16_t rssi_dbm = 0, int16_t snr_quarter_db = 0) {
    (void) event;
    (void) payload_type;
    (void) rssi_dbm;
    (void) snr_quarter_db;
  }
  virtual void loop() = 0;
};
