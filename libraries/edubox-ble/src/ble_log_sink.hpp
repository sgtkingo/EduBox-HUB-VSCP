#pragma once

#include <cstdint>

namespace edubox { namespace ble {

enum class LogLevel : uint8_t {
  Important = 2,
  Detail = 3,
};

// Application-owned diagnostic output.  The BLE library stays independent of
// Serial and of the panel logger, and can therefore also be reused by a Board.
class LogSink {
public:
  virtual ~LogSink() = default;
  virtual bool enabled(LogLevel level) const = 0;
  virtual void write(LogLevel level, const char* source, const char* reason,
                     const char* message) = 0;
};

}} // namespace edubox::ble
