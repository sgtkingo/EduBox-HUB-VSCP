#ifdef ARDUINO_ARCH_ESP32
#include "ble_peripheral.hpp"

namespace edubox { namespace ble {
bool Peripheral::begin(const char* boardId, uint32_t pairingPin, bool forgetBond, uint32_t window) {
  if (!preferences_.begin("edubox-ble", false)) return false;
  if (forgetBond && !preferences_.clear()) return false;
  if (!boardId || !boardId[0] || pairingPin < 100000 || pairingPin > 999999) return false;
  pin_ = pairingPin;
  trusted_ = preferences_.getString("peer", "").c_str();
  trustedType_ = preferences_.getUChar("type", 0);
  pairingUntil_ = millis() + window;
  if (!NimBLEDevice::init(boardId)) return false;
  if (forgetBond && !NimBLEDevice::deleteAllBonds()) return false;
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin_);
  server_ = NimBLEDevice::createServer();
  if (!server_) return false;
  server_->setCallbacks(this, false);
  server_->advertiseOnDisconnect(false); // Main-loop cleanup precedes readvertising.
  auto* service = server_->createService(ServiceUuid);
  auto* rx = service->createCharacteristic(RxUuid, NIMBLE_PROPERTY::WRITE |
      NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, MaxPacket);
  tx_ = service->createCharacteristic(TxUuid, NIMBLE_PROPERTY::INDICATE, MaxPacket);
  status_ = service->createCharacteristic(StatusUuid, NIMBLE_PROPERTY::READ |
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN, 32);
  status_->setValue("EDUBOX-BLE/1;ready=0");
  rx->setCallbacks(this); tx_->setCallbacks(this);
  if (!server_->start()) return false;
  auto* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(ServiceUuid);
  advertising->enableScanResponse(true);
  advertising->setName(boardId);
  return advertising->start();
}
void Peripheral::onConnect(NimBLEServer* server, NimBLEConnInfo& info) {
  bool reject;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    reject = handle_ != BLE_HS_CONN_HANDLE_NONE ||
        (trusted_.empty() && int32_t(millis() - pairingUntil_) >= 0);
    if (!reject) {
      handle_ = info.getConnHandle(); mtu_ = info.getMTU();
      authenticated_ = subscribed_ = savePeer_ = waiting_ = false;
      acknowledgement_ = 0;
    }
  }
  if (reject) { server->disconnect(info.getConnHandle()); return; }
  server->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
}
void Peripheral::onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() != handle_) return;
  authenticated_ = subscribed_ = savePeer_ = waiting_ = false;
  handle_ = BLE_HS_CONN_HANDLE_NONE;
  channel_.disconnect();
}
void Peripheral::onMTUChange(uint16_t mtu, NimBLEConnInfo& info) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() == handle_) mtu_ = mtu;
}
void Peripheral::onAuthenticationComplete(NimBLEConnInfo& info) {
  bool reject;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto id = info.getIdAddress();
    reject = info.getConnHandle() != handle_ || !info.isEncrypted() ||
        !info.isAuthenticated() || !info.isBonded() || info.getSecKeySize() != 16 ||
        (!trusted_.empty() && (trusted_ != id.toString() || trustedType_ != id.getType()));
    if (!reject) {
      candidate_ = id.toString(); candidateType_ = id.getType();
      savePeer_ = trusted_.empty();
      authenticated_ = true;
    }
  }
  if (reject) server_->disconnect(info.getConnHandle());
}
void Peripheral::onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t value) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() != handle_) return;
  subscribed_ = value == 2;
  // CCCD disable is a normal part of client teardown; poll() closes the BLE link.
  if (!subscribed_ && channel_.online()) channel_.disconnect();
}
void Peripheral::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() != handle_ || !authenticated_ || !subscribed_ ||
      !info.isEncrypted() || !info.isAuthenticated()) return;
  const auto value = characteristic->getValue();
  channel_.receive(value.data(), value.size(), channel_.generation(), millis());
}
void Peripheral::onStatus(NimBLECharacteristic*, NimBLEConnInfo&, int code) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  // NimBLE-Arduino 2.5.1 invokes this overload for NOTIFY_TX with a default
  // NimBLEConnInfo (the event's connection handle is not copied into it).
  // This callback belongs exclusively to our TX characteristic and the build
  // permits one BLE connection, so waiting_ is the reliable transaction guard.
  if (waiting_ && code != 0)
    acknowledgement_ = code; // 0 = queued, EDONE = peer confirmation.
}
bool Peripheral::poll() {
  if (!server_) return false;
  channel_.expire(millis());
  const bool lost = channel_.takeLoss();
  uint16_t handle;
  bool send = false, disconnect = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    handle = handle_;
    if (lost) {
      status_->setValue("EDUBOX-BLE/1;ready=0");
      authenticated_ = subscribed_ = false;
      waiting_ = false; disconnect = handle != BLE_HS_CONN_HANDLE_NONE;
    }
    if (!lost && authenticated_ && subscribed_ && !channel_.online()) {
      if (savePeer_) {
        const bool saved = preferences_.putUChar("type", candidateType_) &&
            preferences_.putString("peer", candidate_.c_str());
        if (!saved) { channel_.fault(); disconnect = true; }
        else { trusted_ = candidate_; trustedType_ = candidateType_; savePeer_ = false; }
      }
      if (!disconnect && channel_.open()) status_->setValue("EDUBOX-BLE/1;ready=1");
    }
    if (waiting_) {
      if (acknowledgement_ == BLE_HS_EDONE) { channel_.confirm(pending_); waiting_ = false; }
      else if (acknowledgement_ || uint32_t(millis() - pendingAt_) >= AckTimeoutMs) {
        channel_.fault(); waiting_ = false; disconnect = true;
      }
    }
    if (!disconnect && !waiting_ && channel_.packet(mtu_, pending_)) {
      waiting_ = send = true; acknowledgement_ = 0; pendingAt_ = millis();
    }
  }
  if (disconnect) {
    { std::lock_guard<std::mutex> lock(stateMutex_); authenticated_ = subscribed_ = false; }
    server_->disconnect(handle);
  }
  if (send && !tx_->indicate(pending_.data.data(), pending_.size, handle)) {
    channel_.fault();
    { std::lock_guard<std::mutex> lock(stateMutex_); authenticated_ = subscribed_ = false; }
    server_->disconnect(handle);
  }
  if (handle == BLE_HS_CONN_HANDLE_NONE && !NimBLEDevice::getAdvertising()->isAdvertising())
    NimBLEDevice::getAdvertising()->start();
  return channel_.takeLoss() || lost; // Also deliver faults detected by this poll immediately.
}
bool Peripheral::paired() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return !trusted_.empty();
}

bool Peripheral::openPairingWindow(uint32_t pairingWindowMs) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!trusted_.empty()) return false;
    pairingUntil_ = millis() + pairingWindowMs;
  }
  auto* advertising = NimBLEDevice::getAdvertising();
  return advertising->isAdvertising() || advertising->start();
}

bool Peripheral::forgetBond(uint32_t pairingWindowMs) {
  uint16_t handle;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    handle = handle_;
  }
  channel_.disconnect();
  auto* advertising = NimBLEDevice::getAdvertising();
  // ble_gap_unpair returns EBUSY while advertising or discovery is active.
  if (!advertising->stop()) return false;
  if (server_ && handle != BLE_HS_CONN_HANDLE_NONE) server_->disconnect(handle);
  if (!NimBLEDevice::deleteAllBonds()) {
    advertising->start();
    return false;
  }
  if (!preferences_.clear()) {
    advertising->start();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    handle_ = BLE_HS_CONN_HANDLE_NONE;
    authenticated_ = subscribed_ = savePeer_ = waiting_ = false;
    acknowledgement_ = 0;
    trusted_.clear();
    candidate_.clear();
    trustedType_ = candidateType_ = 0;
    pairingUntil_ = millis() + pairingWindowMs;
  }
  status_->setValue("EDUBOX-BLE/1;ready=0");
  return advertising->isAdvertising() || advertising->start();
}
}}
#endif
