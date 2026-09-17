/**
 * @file vscp_transport.hpp
 * @brief Abstract line transport shared by the VSCP client and server.
 */

#pragma once

#include "vscp_log_sink.hpp"
#include "../vscp_platform.hpp"

namespace vscp {

enum class ReadStatus : uint8_t {
  NoData,
  Message,
  MessageTooLong
};

class Transport {
public:
  explicit Transport(LogSink* logSink = nullptr);
  virtual ~Transport() = default;

  ReadStatus readLine(String& message);
  bool writeLine(const String& message);
  void setLogSink(LogSink* logSink) { logSink_ = logSink; }

protected:
  virtual ReadStatus readLineImpl(String& message) = 0;
  virtual bool writeLineImpl(const String& message) = 0;

private:
  void logTrace(const char* direction, const String& message);
  void logError(const String& message);

  LogSink* logSink_;
};

}  // namespace vscp
