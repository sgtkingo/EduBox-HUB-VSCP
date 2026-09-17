#pragma once
#ifdef ARDUINO_ARCH_ESP32
#include "ble_channel.hpp"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <array>
#include <atomic>
#include <string>

namespace edubox { namespace ble {
enum class LinkState : uint8_t { Off, Idle, Scanning, Connecting, Securing, Ready, Retry, Error };
struct Peer { char address[18]{}; char name[40]{}; uint8_t type = 0; int rssi = 0; };
struct Snapshot {
  LinkState state = LinkState::Off;
  std::array<Peer, 8> peers{};
  size_t count = 0;
  char savedAddress[18]{}, error[96]{};
  uint16_t mtu = 23;
};
// Worker owns blocking GAP/GATT API; callbacks never call VSCP/LVGL.
class Central : private NimBLEClientCallbacks {
  enum class Command { None, Scan, Connect, Saved, Stop, Forget };
  Channel& channel_;
  mutable std::mutex stateMutex_;
  Snapshot snapshot_;
  Command command_ = Command::None;
  Peer target_;
  uint32_t pin_ = 0;
  std::atomic<uint32_t> pairingPin_{0};
  std::atomic<bool> disconnected_{false};
  std::atomic<uint32_t> requestEpoch_{1};
  uint32_t enabledEpoch_ = 1;
  void newRequest() { if (++requestEpoch_ == 0) ++requestEpoch_; }
  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* rx_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  Preferences preferences_;
  Peer saved_;
  bool enabled_ = false;
  uint32_t retryAt_ = 0, backoff_ = 1000;
  void onPassKeyEntry(NimBLEConnInfo&) override;
  void onDisconnect(NimBLEClient*, int) override;
  void onAuthenticationComplete(NimBLEConnInfo&) override;
  void run();
  bool connect(const Peer&, uint32_t pin, uint32_t epoch);
  void setState(LinkState, const char* error = "", uint32_t epoch = 0);
  void stopLink();
  static void task(void* self) { static_cast<Central*>(self)->run(); }
public:
  explicit Central(Channel& channel) : channel_(channel) {}
  bool begin();
  void scan();
  bool select(size_t index, uint32_t pin);
  bool connectSaved();
  void stop();
  void forget();
  Snapshot snapshot() const { std::lock_guard<std::mutex> lock(stateMutex_); return snapshot_; }
};
}}
#endif
