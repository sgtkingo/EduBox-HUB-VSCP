#ifdef ARDUINO_ARCH_ESP32
#include "ble_peripheral.hpp"
#include <esp_system.h>

namespace edubox { namespace ble {
bool Peripheral::begin(const char* name, bool forgetBond, uint32_t window) {
  if (!preferences_.begin("edubox-ble", false)) return false;
  if (forgetBond && !preferences_.clear()) return false;
  pin_ = preferences_.getUInt("pin", 0);
  if (pin_ < 100000 || pin_ > 999999) {
    pin_ = 100000 + esp_random() % 900000;
    if (!preferences_.putUInt("pin", pin_)) return false;
  }
  trusted_ = preferences_.getString("peer", "").c_str();
  trustedType_ = preferences_.getUChar("type", 0);
  pairingUntil_ = millis() + window;
  if (!NimBLEDevice::init(name)) return false;
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
  advertising->setName(name);
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
  if (!subscribed_ && channel_.online()) channel_.fault();
}
void Peripheral::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() != handle_ || !authenticated_ || !subscribed_ ||
      !info.isEncrypted() || !info.isAuthenticated()) return;
  const auto value = characteristic->getValue();
  channel_.receive(value.data(), value.size(), channel_.generation(), millis());
}
void Peripheral::onStatus(NimBLECharacteristic*, NimBLEConnInfo& info, int code) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (info.getConnHandle() == handle_ && waiting_ && code != 0)
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
bool Peripheral::forgetBond() {
  channel_.disconnect();
  return NimBLEDevice::deleteAllBonds() && preferences_.clear();
}
}}
#endif
