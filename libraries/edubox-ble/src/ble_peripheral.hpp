#pragma once
#ifdef ARDUINO_ARCH_ESP32
#include "ble_channel.hpp"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>
#include <string>

namespace edubox { namespace ble {
// BLE callbacks only update link state / bounded Channel. Application poll()
// consumes loss BEFORE Server::poll and router.notifyTransportDisconnected().
class Peripheral : private NimBLEServerCallbacks, private NimBLECharacteristicCallbacks {
  Channel& channel_;
  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* tx_ = nullptr;
  NimBLECharacteristic* status_ = nullptr;
  Preferences preferences_;
  std::mutex stateMutex_;
  uint16_t handle_ = BLE_HS_CONN_HANDLE_NONE, mtu_ = 23;
  uint32_t pin_ = 0, pairingUntil_ = 0, pendingAt_ = 0;
  std::string trusted_, candidate_;
  uint8_t trustedType_ = 0, candidateType_ = 0;
  bool authenticated_ = false, subscribed_ = false, savePeer_ = false, waiting_ = false;
  int acknowledgement_ = 0;
  Packet pending_;
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override;
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override;
  void onMTUChange(uint16_t, NimBLEConnInfo&) override;
  uint32_t onPassKeyDisplay() override { return pin_; }
  void onAuthenticationComplete(NimBLEConnInfo&) override;
  void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) override;
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) override;
  void onStatus(NimBLECharacteristic*, NimBLEConnInfo&, int) override;
public:
  explicit Peripheral(Channel& channel) : channel_(channel) {}
  bool begin(const char* name, bool forgetBond = false, uint32_t pairingWindowMs = 120000);
  uint32_t pairingPin() const { return pin_; } // Local commissioning console only.
  // Returns true once per loss/fault. Caller MUST clean VSCP/devices immediately.
  bool poll();
  bool forgetBond(); // Main-loop local physical commissioning only.
};
}}
#endif
