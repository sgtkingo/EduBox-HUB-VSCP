#ifdef ARDUINO_ARCH_ESP32
#include "ble_central.hpp"
#include "ble_pairing_policy.hpp"
#include <cstdarg>
#include <cstdio>

namespace edubox { namespace ble {
namespace {
template<size_t N> void copy(char (&target)[N], const char* source) {
  std::snprintf(target, N, "%s", source);
}
const char* stateName(LinkState state) {
  switch (state) {
    case LinkState::Off: return "off";
    case LinkState::Idle: return "idle";
    case LinkState::Scanning: return "scanning";
    case LinkState::Connecting: return "connecting";
    case LinkState::Securing: return "securing";
    case LinkState::Forgetting: return "forgetting";
    case LinkState::Ready: return "ready";
    case LinkState::Retry: return "retry";
    case LinkState::Error: return "error";
  }
  return "unknown";
}
}
bool Central::begin() {
  if (worker_) {
    log(LogLevel::Detail, "worker start", "worker already running");
    return true;
  }
  log(LogLevel::Important, "worker start", "creating BLE worker core=0 stack=8192 priority=1");
  const bool created = xTaskCreatePinnedToCore(task, "edubox-ble", 8192, this, 1, &worker_, 0) == pdPASS;
  log(LogLevel::Important, "worker start", "worker create result=%s", created ? "ok" : "failed");
  return created;
}
void Central::log(LogLevel level, const char* reason, const char* format, ...) const {
  if (!logSink_ || !logSink_->enabled(level)) return;
  char message[224];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format ? format : "", args);
  va_end(args);
  logSink_->write(level, "BLE.Central", reason, message);
}
void Central::setState(LinkState state, const char* error, uint32_t epoch) {
  LinkState previous;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (epoch && epoch != requestEpoch_.load()) {
      log(LogLevel::Detail, "state ignored", "stale epoch=%lu current=%lu target=%s",
          static_cast<unsigned long>(epoch), static_cast<unsigned long>(requestEpoch_.load()), stateName(state));
      return;
    }
    previous = snapshot_.state;
    snapshot_.state = state;
    copy(snapshot_.error, error);
  }
  if (previous != state || (error && error[0])) {
    log(LogLevel::Important, "state", "%s -> %s epoch=%lu error=%s", stateName(previous), stateName(state),
        static_cast<unsigned long>(epoch ? epoch : requestEpoch_.load()), error && error[0] ? error : "-");
  }
}
void Central::scan() {
  channel_.disconnect();
  uint32_t epoch;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    newRequest(); epoch = requestEpoch_.load();
    snapshot_.state = LinkState::Scanning; snapshot_.error[0] = 0;
    command_ = Command::Scan;
  }
  log(LogLevel::Important, "scan request", "queued epoch=%lu", static_cast<unsigned long>(epoch));
}
bool Central::select(size_t index, uint32_t pin) {
  Peer selected;
  uint32_t epoch;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (index >= snapshot_.count) {
      log(LogLevel::Important, "connect request", "rejected invalid peer index=%u count=%u",
          static_cast<unsigned>(index), static_cast<unsigned>(snapshot_.count));
      return false;
    }
    if (!validCommissioningPin(pin)) {
      log(LogLevel::Important, "connect request", "rejected invalid commissioning PIN format index=%u",
          static_cast<unsigned>(index));
      return false;
    }
    newRequest(); epoch = requestEpoch_.load();
    channel_.disconnect();
    snapshot_.state = LinkState::Connecting; snapshot_.error[0] = 0;
    target_ = snapshot_.peers[index]; selected = target_;
    pin_ = pin; command_ = Command::Connect;
  }
  log(LogLevel::Important, "connect request", "manual index=%u epoch=%lu",
      static_cast<unsigned>(index), static_cast<unsigned long>(epoch));
  log(LogLevel::Detail, "connect request", "manual peer=%s name=%s addressType=%u",
      selected.address, selected.name[0] ? selected.name : "-", static_cast<unsigned>(selected.type));
  return true;
}
bool Central::connectSaved() {
  Peer selected;
  uint32_t epoch;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!snapshot_.savedAddress[0]) {
      log(LogLevel::Important, "connect request", "rejected: no remembered peer");
      return false;
    }
    newRequest(); epoch = requestEpoch_.load();
    channel_.disconnect();
    snapshot_.state = LinkState::Connecting; snapshot_.error[0] = 0;
    target_ = saved_; selected = target_; command_ = Command::Saved;
  }
  log(LogLevel::Important, "connect request", "remembered epoch=%lu", static_cast<unsigned long>(epoch));
  log(LogLevel::Detail, "connect request", "remembered peer=%s addressType=%u", selected.address,
      static_cast<unsigned>(selected.type));
  return true;
}
void Central::stop() {
  channel_.disconnect();
  std::lock_guard<std::mutex> lock(stateMutex_);
  newRequest(); snapshot_.state = LinkState::Idle; command_ = Command::Stop;
  log(LogLevel::Important, "stop request", "queued epoch=%lu", static_cast<unsigned long>(requestEpoch_.load()));
}
void Central::forget() {
  channel_.disconnect();
  std::lock_guard<std::mutex> lock(stateMutex_);
  newRequest(); snapshot_.state = LinkState::Forgetting; snapshot_.error[0] = 0; command_ = Command::Forget;
  log(LogLevel::Important, "forget request", "queued epoch=%lu", static_cast<unsigned long>(requestEpoch_.load()));
}
void Central::onPassKeyEntry(NimBLEConnInfo& info) {
  const uint32_t pin = pairingPin_.load();
  if (!mayEnterPasskey(pin, pairingEpoch_.load(), requestEpoch_.load())) {
    log(LogLevel::Important, "pairing", "passkey request rejected handle=%u stale-or-missing commissioning request",
        static_cast<unsigned>(info.getConnHandle()));
    client_->disconnect(); return; // Lost bond / cancelled request: require manual commissioning.
  }
  log(LogLevel::Detail, "pairing", "supplying commissioning passkey handle=%u epoch=%lu",
      static_cast<unsigned>(info.getConnHandle()), static_cast<unsigned long>(pairingEpoch_.load()));
  NimBLEDevice::injectPassKey(info, pin);
}
void Central::onConfirmPasskey(NimBLEConnInfo& info, uint32_t) {
  // This profile uses KEYBOARD_ONLY + Board DISPLAY_ONLY, not numeric comparison.
  // Never inherit NimBLE's automatic confirmation of an unexpected association.
  log(LogLevel::Important, "pairing", "unexpected numeric-comparison request rejected handle=%u",
      static_cast<unsigned>(info.getConnHandle()));
  NimBLEDevice::injectConfirmPasskey(info, false);
  client_->disconnect();
}
void Central::onDisconnect(NimBLEClient*, int reason) {
  log(LogLevel::Important, "disconnect callback", "peer disconnected reason=%d", reason);
  channel_.disconnect(); disconnected_.store(true);
}
void Central::onAuthenticationComplete(NimBLEConnInfo& info) {
  const bool accepted = info.isEncrypted() && info.isAuthenticated() && info.isBonded() && info.getSecKeySize() == 16;
  log(accepted ? LogLevel::Detail : LogLevel::Important, "authentication",
      "complete handle=%u encrypted=%d authenticated=%d bonded=%d keySize=%u accepted=%d",
      static_cast<unsigned>(info.getConnHandle()), info.isEncrypted(), info.isAuthenticated(), info.isBonded(),
      static_cast<unsigned>(info.getSecKeySize()), accepted);
  if (!accepted)
    client_->disconnect();
}
void Central::stopLink() {
  log(LogLevel::Detail, "link cleanup", "begin clientConnected=%d channelOnline=%d", client_->isConnected(), channel_.online());
  rx_ = nullptr;
  if (channel_.online()) channel_.disconnect();
  if (client_->isConnected()) client_->disconnect();
  const uint32_t started = millis();
  while (client_->isConnected() && uint32_t(millis() - started) < 2500)
    vTaskDelay(pdMS_TO_TICKS(10)); // Worker only; never proceed on the previous peer.
  log(LogLevel::Detail, "link cleanup", "complete clientConnected=%d elapsedMs=%lu", client_->isConnected(),
      static_cast<unsigned long>(millis() - started));
}
bool Central::connect(const Peer& peer, uint32_t pin, uint32_t epoch) {
  auto publish = [this, epoch](LinkState state, const char* error = "") { setState(state, error, epoch); };
  auto cancelled = [this, epoch] { return epoch != requestEpoch_.load(); };
  log(LogLevel::Important, "connect", "begin mode=%s epoch=%lu", pin ? "commissioning" : "bonded",
      static_cast<unsigned long>(epoch));
  log(LogLevel::Detail, "connect", "target peer=%s addressType=%u", peer.address,
      static_cast<unsigned>(peer.type));
  if (cancelled()) {
    log(LogLevel::Detail, "connect", "cancelled before GAP connect epoch=%lu", static_cast<unsigned long>(epoch));
    return false;
  }
  publish(LinkState::Connecting);
  if (client_->isConnected()) {
    publish(LinkState::Error, "Previous peer disconnect pending"); return false;
  }
  if (!pin && !NimBLEDevice::isBonded(NimBLEAddress(peer.address, peer.type))) {
    publish(LinkState::Error, "Saved bond missing: Scan and pair manually"); return false;
  }
  pairingEpoch_.store(epoch);
  pairingPin_.store(pin);
  if (!client_->connect(NimBLEAddress(peer.address, peer.type), true)) {
    publish(LinkState::Error, "Board unavailable"); return false;
  }
  log(LogLevel::Detail, "connect", "GAP connected peer=%s mtu=%u", peer.address,
      static_cast<unsigned>(client_->getMTU()));
  if (cancelled()) {
    log(LogLevel::Detail, "connect", "cancelled after GAP connect epoch=%lu", static_cast<unsigned long>(epoch));
    stopLink(); return false;
  }
  publish(LinkState::Securing);
  if (!client_->secureConnection()) { stopLink(); publish(LinkState::Error, "Pairing failed: check PIN / BOOT reset"); return false; }
  if (cancelled()) {
    log(LogLevel::Detail, "connect", "cancelled after security procedure epoch=%lu", static_cast<unsigned long>(epoch));
    stopLink(); return false;
  }
  const auto info = client_->getConnInfo();
  const auto identity = info.getIdAddress();
  log(LogLevel::Detail, "security", "peer=%s identity=%s identityType=%u encrypted=%d authenticated=%d bonded=%d keySize=%u",
      peer.address, identity.toString().c_str(), static_cast<unsigned>(identity.getType()), info.isEncrypted(),
      info.isAuthenticated(), info.isBonded(), static_cast<unsigned>(info.getSecKeySize()));
  if (!info.isEncrypted() || !info.isAuthenticated() || !info.isBonded() || info.getSecKeySize() != 16) {
    stopLink(); publish(LinkState::Error, "Authenticated encryption required"); return false;
  }
  if (!acceptsPeerIdentity(pin, peer.address, peer.type, identity.toString().c_str(), identity.getType())) {
    stopLink(); publish(LinkState::Error, "Bonded Board identity mismatch"); return false;
  }
  auto* service = client_->getService(ServiceUuid);
  if (!service) { stopLink(); publish(LinkState::Error, "Not an EduBox Board"); return false; }
  log(LogLevel::Detail, "GATT discovery", "EduBox service discovered peer=%s", peer.address);
  auto* tx = service->getCharacteristic(TxUuid);
  auto* status = service->getCharacteristic(StatusUuid);
  rx_ = service->getCharacteristic(RxUuid);
  if (!rx_ || !rx_->canWrite() || !tx || !tx->canIndicate() || !status || !status->canRead()) {
    stopLink(); publish(LinkState::Error, "Incompatible BLE service"); return false;
  }
  log(LogLevel::Detail, "GATT discovery", "characteristics valid rxWrite=%d txIndicate=%d statusRead=%d",
      rx_->canWrite(), tx->canIndicate(), status->canRead());
  // Main loop must invalidate preceding VSCP session before reopening Channel.
  const uint32_t cleanupAt = millis();
  while (!channel_.open()) {
    if (cancelled() || !client_->isConnected() || uint32_t(millis() - cleanupAt) >= 3000) {
      stopLink(); publish(LinkState::Error, "Session cleanup timeout"); return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  const uint32_t generation = channel_.generation();
  log(LogLevel::Detail, "channel", "opened generation=%lu", static_cast<unsigned long>(generation));
  if (!tx->subscribe(false, [this, generation](NimBLERemoteCharacteristic*, uint8_t* data, size_t size, bool notify) {
        if (!notify) channel_.receive(data, size, generation, millis());
      }, true)) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Indication subscription failed"); return false;
  }
  log(LogLevel::Detail, "GATT subscribe", "TX indications subscribed generation=%lu", static_cast<unsigned long>(generation));
  const uint32_t start = millis();
  bool ready = false;
  while (!cancelled() && client_->isConnected() && uint32_t(millis() - start) < 3000) {
    if (std::strcmp(status->readValue().c_str(), "EDUBOX-BLE/1;ready=1") == 0 && channel_.online()) { ready = true; break; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!ready || cancelled()) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Session cleanup / readiness timeout"); return false;
  }
  log(LogLevel::Detail, "readiness", "Board ready elapsedMs=%lu mtu=%u", static_cast<unsigned long>(millis() - start),
      static_cast<unsigned>(client_->getMTU()));
  // Persist only an authenticated identity, never the entered PIN.
  const auto id = info.getIdAddress();
  bool saved = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (epoch == requestEpoch_.load()) {
      saved = preferences_.putUChar("type", id.getType()) &&
          preferences_.putString("peer", id.toString().c_str()) &&
          preferences_.putString("name", peer.name);
      if (saved) {
        saved_ = peer; copy(saved_.address, id.toString().c_str()); saved_.type = id.getType();
        copy(snapshot_.savedAddress, saved_.address); copy(snapshot_.savedBoardId, saved_.name);
        snapshot_.mtu = client_->getMTU();
      }
    }
  }
  if (!saved) {
    channel_.fault(); stopLink(); publish(LinkState::Error, "Unable to save bonded peer / cancelled"); return false;
  }
  log(LogLevel::Detail, "bond storage", "saved authenticated identity=%s type=%u",
      id.toString().c_str(), static_cast<unsigned>(id.getType()));
  backoff_ = 1000;
  if (cancelled()) { stopLink(); return false; }
  publish(LinkState::Ready);
  log(LogLevel::Important, "connect", "ready mtu=%u epoch=%lu", static_cast<unsigned>(client_->getMTU()),
      static_cast<unsigned long>(epoch));
  return true;
}
void Central::run() {
  log(LogLevel::Important, "initialization", "BLE worker running; opening preferences");
  if (!preferences_.begin("edubox-ble", false)) {
    setState(LinkState::Error, "BLE preferences initialization failed");
    log(LogLevel::Important, "initialization", "preferences open failed; worker stopping");
    vTaskDelete(nullptr); return;
  }
  log(LogLevel::Detail, "initialization", "preferences opened; initializing NimBLE");
  if (!NimBLEDevice::init("EduBox-HUB-Panel")) {
    setState(LinkState::Error, "BLE stack initialization failed");
    log(LogLevel::Important, "initialization", "NimBLE init failed; worker stopping");
    vTaskDelete(nullptr); return;
  }
  copy(saved_.address, preferences_.getString("peer", "").c_str());
  copy(saved_.name, preferences_.getString("name", "").c_str());
  saved_.type = preferences_.getUChar("type", 0);
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    copy(snapshot_.savedAddress, saved_.address); copy(snapshot_.savedBoardId, saved_.name);
  }
  log(LogLevel::Important, "initialization", "NimBLE ready rememberedPeer=%d", saved_.address[0] != 0);
  if (saved_.address[0]) {
    log(LogLevel::Detail, "initialization", "remembered boardId=%s address=%s type=%u",
        saved_.name[0] ? saved_.name : "-", saved_.address, static_cast<unsigned>(saved_.type));
  }
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  client_ = NimBLEDevice::createClient();
  if (!client_) {
    setState(LinkState::Error, "BLE client allocation failed");
    log(LogLevel::Important, "initialization", "client allocation failed; worker stopping");
    vTaskDelete(nullptr); return;
  }
  client_->setClientCallbacks(this, false);
  client_->setConnectTimeout(5000);
  client_->setConnectionParams(12, 24, 0, 200);
  auto* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true);
  // Each scan is bounded to five seconds below.  Completing the scan flushes
  // advertisements still waiting for a scan response, so NimBLE's default
  // 10.24-second response timeout cannot improve these results.  Disabling it
  // also avoids a lazy esp_timer allocation after the GUI has consumed most of
  // the internal heap.
  scanner->setScanResponseTimeout(0);
  scanner->setMaxResults(32);
  log(LogLevel::Detail, "initialization", "client/scanner configured mtu=247 activeScan=1 maxResults=32 responseTimer=off");
  setState(LinkState::Idle);
  for (;;) {
    Command command; Peer target; uint32_t pin, epoch;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      command = command_; command_ = Command::None; target = target_; pin = pin_;
      epoch = requestEpoch_.load();
      pin_ = 0;
    }
    if (command != Command::None) {
      const char* name = command == Command::Scan ? "scan" : command == Command::Connect ? "connect" :
          command == Command::Saved ? "saved" : command == Command::Stop ? "stop" : "forget";
      log(LogLevel::Detail, "command", "processing command=%s epoch=%lu", name, static_cast<unsigned long>(epoch));
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
      saved_ = Peer(); snapshot_.savedAddress[0] = 0; snapshot_.savedBoardId[0] = 0;
      log(LogLevel::Important, "bond storage", "all local BLE bonds and remembered identity erased");
    }
    if (command == Command::Stop || command == Command::Forget) setState(LinkState::Idle, "", epoch);
    if (command == Command::Scan) {
      setState(LinkState::Scanning, "", epoch);
      log(LogLevel::Important, "scan", "started durationMs=5000 epoch=%lu", static_cast<unsigned long>(epoch));
      const auto results = scanner->getResults(5000);
      Snapshot found; found.state = LinkState::Idle;
      for (int i = 0; i < results.getCount() && found.count < found.peers.size(); ++i) {
        const auto* advertised = results.getDevice(i);
        const bool serviceMatch = advertised->isAdvertisingService(NimBLEUUID(ServiceUuid));
        log(LogLevel::Detail, "scan result", "index=%d address=%s type=%u name=%s rssi=%d serviceMatch=%d",
            i, advertised->getAddress().toString().c_str(), static_cast<unsigned>(advertised->getAddress().getType()),
            advertised->getName().empty() ? "-" : advertised->getName().c_str(), advertised->getRSSI(), serviceMatch);
        if (!serviceMatch) continue;
        auto& peer = found.peers[found.count++];
        copy(peer.address, advertised->getAddress().toString().c_str());
        copy(peer.name, advertised->getName().c_str());
        for (auto& character : peer.name)
          if (character && (uint8_t(character) < 32 || uint8_t(character) > 126)) character = '_';
        peer.type = advertised->getAddress().getType(); peer.rssi = advertised->getRSSI();
      }
      log(LogLevel::Important, "scan", "completed received=%d matched=%u epoch=%lu", results.getCount(),
          static_cast<unsigned>(found.count), static_cast<unsigned long>(epoch));
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (epoch == requestEpoch_.load()) {
          snapshot_.peers = found.peers; snapshot_.count = found.count;
          snapshot_.state = LinkState::Idle;
          copy(snapshot_.error, found.count ? "" : "No Board found (pairing window / range)");
        }
      }
      scanner->clearResults();
      log(LogLevel::Detail, "scan", "NimBLE scan results released");
    }
    if (command == Command::Connect || command == Command::Saved) {
      if (command == Command::Saved) pin = 0; // Target captured when the user requested it.
      enabled_ = true;
      enabledEpoch_ = epoch;
      if (!target.address[0] || !connect(target, pin, epoch) || epoch != requestEpoch_.load()) {
        enabled_ = false; // Pairing/connect errors need explicit user action.
        log(LogLevel::Important, "connect", "attempt ended without ready state epoch=%lu", static_cast<unsigned long>(epoch));
      }
      pairingPin_.store(0); pin = 0; // Retain PIN only for the pending pairing.
      pairingEpoch_.store(0);
      disconnected_.store(false);
    }
    channel_.expire(millis());
    if (enabled_ && enabledEpoch_ != requestEpoch_.load()) { enabled_ = false; stopLink(); }
    const bool disconnected = disconnected_.exchange(false);
    if (enabled_ && (disconnected || (client_->isConnected() && !channel_.online()))) {
      stopLink(); retryAt_ = millis() + backoff_;
      log(LogLevel::Important, "reconnect", "link lost; retry scheduled in %lu ms epoch=%lu",
          static_cast<unsigned long>(backoff_), static_cast<unsigned long>(enabledEpoch_));
      backoff_ = backoff_ < 8000 ? backoff_ * 2 : 8000;
      if (enabled_) setState(LinkState::Retry, "Link lost; VSCP reconnect required", enabledEpoch_);
    }
    if (enabled_ && !client_->isConnected() && int32_t(millis() - retryAt_) >= 0) {
      log(LogLevel::Important, "reconnect", "attempting remembered peer epoch=%lu",
          static_cast<unsigned long>(enabledEpoch_));
      log(LogLevel::Detail, "reconnect", "target peer=%s addressType=%u", saved_.address,
          static_cast<unsigned>(saved_.type));
      if (!connect(saved_, 0, enabledEpoch_)) {
        const uint32_t retryDelay = backoff_;
        retryAt_ = millis() + retryDelay; backoff_ = backoff_ < 8000 ? backoff_ * 2 : 8000;
        log(LogLevel::Important, "reconnect", "attempt failed; next retry in %lu ms",
            static_cast<unsigned long>(retryDelay));
        setState(LinkState::Retry, "Saved Board unavailable", enabledEpoch_);
      }
      disconnected_.store(false);
    }
    if (enabled_ && channel_.online() && rx_) {
      Packet packet;
      if (channel_.packet(client_->getMTU(), packet)) {
        if (rx_->writeValue(packet.data.data(), packet.size, true)) channel_.confirm(packet);
        else {
          log(LogLevel::Important, "GATT write", "write failed bytes=%u generation=%lu",
              static_cast<unsigned>(packet.size), static_cast<unsigned long>(packet.generation));
          channel_.fault(); stopLink(); disconnected_.store(true);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}}
#endif
