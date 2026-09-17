/**
 * @file vscp_stream_transport.hpp
 * @brief Non-blocking VSCP framing adapter for an Arduino Stream.
 */

#pragma once

#include "../config.hpp"

#ifdef ARDUINO_H_ENV

#include "../vscp_types.hpp"
#include "vscp_transport.hpp"

#include <Stream.h>

namespace vscp {

class StreamLogSink : public LogSink {
public:
  explicit StreamLogSink(Print& output) : output_(output) {}
  void writeLogLine(const String& message) override { output_.println(message); }

private:
  Print& output_;
};

class StreamTransport : public Transport {
public:
  explicit StreamTransport(Stream& stream, size_t maxMessageSize = MAX_MESSAGE_SIZE,
                           LogSink* logSink = nullptr);
  // Main-owner only: forget partial framing and currently buffered physical RX.
  void clearInput() {
    buffer_ = ""; overflowed_ = false;
    int remaining = stream_.available();
    while (remaining-- > 0) stream_.read();
  }

protected:
  ReadStatus readLineImpl(String& message) override;
  bool writeLineImpl(const String& message) override;

private:
  Stream& stream_;
  String buffer_;
  size_t maxMessageSize_;
  bool overflowed_ = false;
};

}  // namespace vscp

#endif
