#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace edubox { namespace ble {
// Transport envelope v1. Each direction is strictly ordered, acknowledged ATT.
// Header: EB,01, messageId LE16, offset LE16, total LE16. No newline on GATT.
constexpr size_t MaxLine = 1024, QueueDepth = 4, HeaderSize = 8, MaxPacket = 244;
constexpr uint32_t FragmentTimeoutMs = 2000;
constexpr const char* ServiceUuid = "ecb00001-8b65-4f41-9f17-6a672d8ad701";
constexpr const char* RxUuid      = "ecb00002-8b65-4f41-9f17-6a672d8ad701";
constexpr const char* TxUuid      = "ecb00003-8b65-4f41-9f17-6a672d8ad701";
constexpr const char* StatusUuid  = "ecb00004-8b65-4f41-9f17-6a672d8ad701";

struct Line { std::array<char, MaxLine + 1> data{}; uint16_t size = 0; };
struct Packet {
  std::array<uint8_t, MaxPacket> data{};
  size_t size = 0, payload = 0;
  uint32_t generation = 0;
};
struct Stats { uint32_t rxLines = 0, txLines = 0, faults = 0, disconnects = 0; };

class Channel {
  struct Queue {
    std::array<Line, QueueDepth> lines{};
    size_t head = 0, count = 0;
    void clear() { head = count = 0; }
    bool push(const char* data, size_t size) {
      if (count == QueueDepth) return false;
      auto& line = lines[(head + count++) % QueueDepth];
      std::memcpy(line.data.data(), data, size);
      line.data[size] = 0; line.size = static_cast<uint16_t>(size); return true;
    }
    Line& front() { return lines[head]; }
    void pop() { head = (head + 1) % QueueDepth; --count; }
  };
  mutable std::mutex mutex_;
  Queue rx_, tx_;
  Line partial_;
  uint16_t rxId_ = 1, txId_ = 1, rxOffset_ = 0, txOffset_ = 0;
  uint32_t generation_ = 0, partialAt_ = 0;
  bool online_ = false, loss_ = false;
  Stats stats_;
  static uint16_t next(uint16_t value) { return value == UINT16_MAX ? 1 : value + 1; }
  static uint16_t get16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
  static void put16(uint8_t* p, uint16_t n) { p[0] = uint8_t(n); p[1] = uint8_t(n >> 8); }
  void clear() { rx_.clear(); tx_.clear(); partial_.size = rxOffset_ = txOffset_ = 0; rxId_ = txId_ = 1; }
  void lose(bool fault) {
    if (fault) ++stats_.faults;
    if (online_) ++stats_.disconnects;
    online_ = false; loss_ = true; ++generation_; clear();
  }
public:
  // Do not reopen until application consumed loss and invalidated VSCP/hardware.
  bool open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (online_ || loss_) return false;
    clear(); ++generation_; online_ = true; return true;
  }
  void disconnect() { std::lock_guard<std::mutex> lock(mutex_); lose(false); }
  void fault() { std::lock_guard<std::mutex> lock(mutex_); lose(true); }
  bool takeLoss() { std::lock_guard<std::mutex> lock(mutex_); bool value = loss_; loss_ = false; return value; }
  bool online() const { std::lock_guard<std::mutex> lock(mutex_); return online_; }
  uint32_t generation() const { std::lock_guard<std::mutex> lock(mutex_); return generation_; }
  Stats stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
  bool enqueue(const char* line, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_) return false;
    if (!size || size > MaxLine) { lose(true); return false; }
    for (size_t i = 0; i < size; ++i)
      if (uint8_t(line[i]) < 32 || uint8_t(line[i]) > 126) { lose(true); return false; }
    if (!tx_.push(line, size)) { lose(true); return false; }
    return true;
  }
  bool read(Line& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || !rx_.count) return false;
    line = rx_.front(); rx_.pop(); return true;
  }
  bool receive(const uint8_t* data, size_t size, uint32_t generation, uint32_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || generation != generation_) return false; // Old callback.
    if (size <= HeaderSize || size > MaxPacket || data[0] != 0xEB || data[1] != 1) { lose(true); return false; }
    const auto id = get16(data + 2), offset = get16(data + 4), total = get16(data + 6);
    const size_t count = size - HeaderSize;
    if (id != rxId_ || offset != rxOffset_ || !total || total > MaxLine ||
        offset + count > total || (rxOffset_ && partial_.size != total)) { lose(true); return false; }
    for (size_t i = HeaderSize; i < size; ++i)
      if (data[i] < 32 || data[i] > 126) { lose(true); return false; }
    if (!rxOffset_) partialAt_ = now;
    partial_.size = total;
    std::memcpy(partial_.data.data() + offset, data + HeaderSize, count);
    rxOffset_ += static_cast<uint16_t>(count);
    if (rxOffset_ == total) {
      if (!rx_.push(partial_.data.data(), total)) { lose(true); return false; }
      ++stats_.rxLines; rxOffset_ = partial_.size = 0; rxId_ = next(rxId_);
    }
    return true;
  }
  bool packet(uint16_t mtu, Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || !tx_.count) return false;
    if (mtu <= 3 + HeaderSize) { lose(true); return false; }
    const size_t attCapacity = size_t(mtu - 3);
    const size_t capacity = (attCapacity < MaxPacket ? attCapacity : MaxPacket) - HeaderSize;
    auto& line = tx_.front();
    const size_t remaining = size_t(line.size - txOffset_);
    packet.payload = remaining < capacity ? remaining : capacity;
    packet.size = HeaderSize + packet.payload; packet.generation = generation_;
    packet.data[0] = 0xEB; packet.data[1] = 1;
    put16(packet.data.data() + 2, txId_); put16(packet.data.data() + 4, txOffset_);
    put16(packet.data.data() + 6, line.size);
    std::memcpy(packet.data.data() + HeaderSize, line.data.data() + txOffset_, packet.payload);
    return true;
  }
  // Advance only after ATT write response / indication confirmation.
  bool confirm(const Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || packet.generation != generation_ || !tx_.count ||
        get16(packet.data.data() + 2) != txId_ || get16(packet.data.data() + 4) != txOffset_) return false;
    txOffset_ += static_cast<uint16_t>(packet.payload);
    if (txOffset_ == tx_.front().size) {
      tx_.pop(); txOffset_ = 0; txId_ = next(txId_); ++stats_.txLines;
    }
    return true;
  }
  void expire(uint32_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (online_ && rxOffset_ && uint32_t(now - partialAt_) >= FragmentTimeoutMs) lose(true);
  }
};
}} // namespace edubox::ble
