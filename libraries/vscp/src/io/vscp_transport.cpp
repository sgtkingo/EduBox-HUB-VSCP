/**
 * @file vscp_transport.cpp
 * @brief Implements shared VSCP transport diagnostics and dispatch.
 */

#include "vscp_transport.hpp"

namespace vscp {

Transport::Transport(LogSink* logSink) : logSink_(logSink) {}

ReadStatus Transport::readLine(String& message) {
  const ReadStatus status = readLineImpl(message);
  if (status == ReadStatus::Message) {
    message = detail::stripMessage(message);
    if (detail::stringLength(message) == 0) return ReadStatus::NoData;
    logTrace("RX", message);
  } else if (status == ReadStatus::MessageTooLong) {
    logError("Incoming message exceeds MAX_PROTOCOL_REQUEST_SIZE");
  }
  return status;
}

bool Transport::writeLine(const String& message) {
  const String cleanMessage = detail::stripMessage(message);
  if (detail::stringLength(cleanMessage) == 0) {
    logError("Refusing to write an empty protocol message");
    return false;
  }

  logTrace("TX", cleanMessage);
  const bool written = writeLineImpl(cleanMessage);
  if (!written) logError("Unable to write protocol message");
  return written;
}

void Transport::logTrace(const char* direction, const String& message) {
#if PROTOCOL_VERBOSE >= 2
  if (logSink_) logSink_->writeLogLine(String("[VSCP][") + direction + "] " + message);
#else
  (void)direction;
  (void)message;
#endif
}

void Transport::logError(const String& message) {
#if PROTOCOL_VERBOSE >= 1
  if (logSink_) logSink_->writeLogLine(String("[VSCP][ERROR] ") + message);
#else
  (void)message;
#endif
}

}  // namespace vscp
