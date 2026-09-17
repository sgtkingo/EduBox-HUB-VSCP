#ifdef ARDUINO_ARCH_ESP32
#include "ble_central.hpp"
#include <cstdio>

namespace edubox { namespace ble {
namespace {
template<size_t N> void copy(char (&target)[N], const char* source) {
  std::snprintf(target, N, "%s", source);
}
}
bool Central::begin() {
  if (worker_) return true;
  return xTaskCreatePinnedToCore(task, "edubox-ble", 8192, this, 1, &worker_, 0) == pdPASS;
}
void Central::setState(LinkState state, const char* error, uint32_t epoch) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (epoch && epoch != requestEpoch_.load()) return;
  snapshot_.state = state; copy(snapshot_.error, error);
}
void Central::scan() {
  channel_.disconnect();
  std::lock_guard<std::mutex> lock(stateMutex_);
  newRequest();
  snapshot_.state = LinkState::Scanning; snapshot_.error[0] = 0;
  command_ = Command::Scan;
}
bool Central::select(size_t index, uint32_t pin) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (index >= snapshot_.count || pin > 999999) return false;
  newRequest();
  channel_.disconnect();
  snapshot_.state = LinkState::Connecting; snapshot_.error[0] = 0;
  target_ = snapshot_.peers[index]; pin_ = pin; command_ = Command::Connect; return true;
}
bool Central::connectSaved() {
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (!snapshot_.savedAddress[0]) return false;
  newRequest();
  channel_.disconnect();
  snapshot_.state = LinkState::Connecting; snapshot_.error[0] = 0;
  target_ = saved_; command_ = Command::Saved; return true;
}
void Central::stop() { channel_.disconnect(); std::lock_guard<std::mutex> lock(stateMutex_); newRequest(); snapshot_.state = LinkState::Idle; command_ = Command::Stop; }
void Central::forget() { channel_.disconnect(); std::lock_guard<std::mutex> lock(stateMutex_); newRequest(); snapshot_.state = LinkState::Idle; command_ = Command::Forget; }
void Central::onPassKeyEntry(NimBLEConnInfo& info) { NimBLEDevice::injectPassKey(info, pairingPin_.load()); }
void Central::onDisconnect(NimBLEClient*, int) { channel_.disconnect(); disconnected_.store(true); }
void Central::onAuthenticationComplete(NimBLEConnInfo& info) {
  if (!info.isEncrypted() || !info.isAuthenticated() || !info.isBonded() || info.getSecKeySize() != 16)
    client_->disconnect();
}
void Central::stopLink() {
  rx_ = nullptr;
  if (channel_.online()) channel_.disconnect();
  if (client_->isConnected()) client_->disconnect();
  const uint32_t started = millis();
  while (client_->isConnected() && uint32_t(millis() - started) < 2500)
    vTaskDelay(pdMS_TO_TICKS(10)); // Worker only; never proceed on the previous peer.
}
bool Central::connect(const Peer& peer, uint32_t pin, uint32_t epoch) {
  auto publish = [this, epoch](LinkState state, const char* error = "") { setState(state, error, epoch); };
  auto cancelled = [this, epoch] { return epoch != requestEpoch_.load(); };
  if (cancelled()) return false;
  publish(LinkState::Connecting);
  if (client_->isConnected()) {
    publish(LinkState::Error, "Previous peer disconnect pending"); return false;
  }
  pairingPin_.store(pin);
  if (!client_->connect(NimBLEAddress(peer.address, peer.type), true)) {
    publish(LinkState::Error, "Board unavailable"); return false;
  }
  if (cancelled()) { stopLink(); return false; }
  publish(LinkState::Securing);
  if (!client_->secureConnection()) { stopLink(); publish(LinkState::Error, "Pairing failed: check PIN / BOOT reset"); return false; }
  if (cancelled()) { stopLink(); return false; }
  const auto info = client_->getConnInfo();
  if (!info.isEncrypted() || !info.isAuthenticated() || !info.isBonded() || info.getSecKeySize() != 16) {
    stopLink(); publish(LinkState::Error, "Authenticated encryption required"); return false;
  }
  auto* service = client_->getService(ServiceUuid);
  if (!service) { stopLink(); publish(LinkState::Error, "Not an EduBox Board"); return false; }
  auto* tx = service->getCharacteristic(TxUuid);
  auto* status = service->getCharacteristic(StatusUuid);
  rx_ = service->getCharacteristic(RxUuid);
  if (!rx_ || !rx_->canWrite() || !tx || !tx->canIndicate() || !status || !status->canRead()) {
    stopLink(); publish(LinkState::Error, "Incompatible BLE service"); return false;
  }
  // Main loop must invalidate preceding VSCP session before reopening Channel.
  const uint32_t cleanupAt = millis();
  while (!channel_.open()) {
    if (cancelled() || !client_->isConnected() || uint32_t(millis() - cleanupAt) >= 3000) {
      stopLink(); publish(LinkState::Error, "Session cleanup timeout"); return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  const uint32_t generation = channel_.generation();
  if (!tx->subscribe(false, [this, generation](NimBLERemoteCharacteristic*, uint8_t* data, size_t size, bool notify) {
        if (!notify) channel_.receive(data, size, generation, millis());
      }, true)) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Indication subscription failed"); return false;
  }
  const uint32_t start = millis();
  bool ready = false;
  while (!cancelled() && client_->isConnected() && uint32_t(millis() - start) < 3000) {
    if (std::strcmp(status->readValue().c_str(), "EDUBOX-BLE/1;ready=1") == 0 && channel_.online()) { ready = true; break; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!ready || cancelled()) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Session cleanup / readiness timeout"); return false;
  }
  // Persist only an authenticated identity, never the entered PIN.
  const auto id = info.getIdAddress();
  bool saved = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (epoch == requestEpoch_.load()) {
      saved = preferences_.putUChar("type", id.getType()) && preferences_.putString("peer", id.toString().c_str());
      if (saved) {
        saved_ = peer; copy(saved_.address, id.toString().c_str()); saved_.type = id.getType();
        copy(snapshot_.savedAddress, saved_.address); snapshot_.mtu = client_->getMTU();
      }
    }
  }
  if (!saved) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Unable to save bonded peer / cancelled"); return false;
  }
  backoff_ = 1000;
  if (cancelled()) { stopLink(); return false; }
  publish(LinkState::Ready);
  return true;
}
void Central::run() {
  if (!preferences_.begin("edubox-ble", false) || !NimBLEDevice::init("EduBox-HUB-Panel")) {
    setState(LinkState::Error, "BLE initialization failed"); vTaskDelete(nullptr); return;
  }
  copy(saved_.address, preferences_.getString("peer", "").c_str());
  saved_.type = preferences_.getUChar("type", 0);
  {
    std::lock_guard<std::mutex> lock(stateMutex_); copy(snapshot_.savedAddress, saved_.address);
  }
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  client_ = NimBLEDevice::createClient();
  if (!client_) { setState(LinkState::Error, "BLE client allocation failed"); vTaskDelete(nullptr); return; }
  client_->setClientCallbacks(this, false);
  client_->setConnectTimeout(5000);
  client_->setConnectionParams(12, 24, 0, 200);
  auto* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true); scanner->setMaxResults(32);
  setState(LinkState::Idle);
  for (;;) {
    Command command; Peer target; uint32_t pin, epoch;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      command = command_; command_ = Command::None; target = target_; pin = pin_;
      epoch = requestEpoch_.load();
      pin_ = 0;
    }
    if (command == Command::Stop || command == Command::Forget || command == Command::Scan ||
        command == Command::Connect || command == Command::Saved) {
      enabled_ = false; stopLink(); disconnected_.store(false);
    }
    if (command == Command::Forget) {
      if (!NimBLEDevice::deleteAllBonds() || !preferences_.clear()) {
        setState(LinkState::Error, "Unable to erase bond; retry locally", epoch);
        vTaskDelay(pdMS_TO_TICKS(5)); continue;
      }
      std::lock_guard<std::mutex> lock(stateMutex_);
      saved_ = Peer(); snapshot_.savedAddress[0] = 0;
    }
    if (command == Command::Stop || command == Command::Forget) setState(LinkState::Idle, "", epoch);
    if (command == Command::Scan) {
      setState(LinkState::Scanning, "", epoch);
      const auto results = scanner->getResults(5000);
      Snapshot found; found.state = LinkState::Idle;
      for (int i = 0; i < results.getCount() && found.count < found.peers.size(); ++i) {
        const auto* advertised = results.getDevice(i);
        if (!advertised->isAdvertisingService(NimBLEUUID(ServiceUuid))) continue;
        auto& peer = found.peers[found.count++];
        copy(peer.address, advertised->getAddress().toString().c_str());
        copy(peer.name, advertised->getName().c_str());
        for (auto& character : peer.name)
          if (character && (uint8_t(character) < 32 || uint8_t(character) > 126)) character = '_';
        peer.type = advertised->getAddress().getType(); peer.rssi = advertised->getRSSI();
      }
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (epoch == requestEpoch_.load()) {
          snapshot_.peers = found.peers; snapshot_.count = found.count;
          snapshot_.state = LinkState::Idle;
          copy(snapshot_.error, found.count ? "" : "No Board found (pairing window / range)");
        }
      }
      scanner->clearResults();
    }
    if (command == Command::Connect || command == Command::Saved) {
      if (command == Command::Saved) pin = 0; // Target captured when the user requested it.
      enabled_ = true;
      enabledEpoch_ = epoch;
      if (!target.address[0] || !connect(target, pin, epoch) || epoch != requestEpoch_.load()) {
        enabled_ = false; // Pairing/connect errors need explicit user action.
      }
      pairingPin_.store(0); pin = 0; // Retain PIN only for the pending pairing.
      disconnected_.store(false);
    }
    channel_.expire(millis());
    if (enabled_ && enabledEpoch_ != requestEpoch_.load()) { enabled_ = false; stopLink(); }
    if (disconnected_.exchange(false) || (enabled_ && client_->isConnected() && !channel_.online())) {
      stopLink(); retryAt_ = millis() + backoff_;
      backoff_ = backoff_ < 8000 ? backoff_ * 2 : 8000;
      if (enabled_) setState(LinkState::Retry, "Link lost; VSCP reconnect required", enabledEpoch_);
    }
    if (enabled_ && !client_->isConnected() && int32_t(millis() - retryAt_) >= 0) {
      if (!connect(saved_, 0, enabledEpoch_)) {
        retryAt_ = millis() + backoff_; backoff_ = backoff_ < 8000 ? backoff_ * 2 : 8000;
        setState(LinkState::Retry, "Saved Board unavailable", enabledEpoch_);
      }
      disconnected_.store(false);
    }
    if (enabled_ && channel_.online() && rx_) {
      Packet packet;
      if (channel_.packet(client_->getMTU(), packet)) {
        if (rx_->writeValue(packet.data.data(), packet.size, true)) channel_.confirm(packet);
        else { channel_.fault(); stopLink(); disconnected_.store(true); }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}}
#endif
