#pragma once

#include "io/vscp_transport.hpp"
#include "vscp_codec.hpp"
#include <cstdio>

namespace vscp {
namespace detail {

// One exchange per transport; all reads remain owned by Client/Server.
class PingExchange {
public:
  explicit PingExchange(bool server) : server_(server) {}

  bool start(Transport& transport, unsigned long timeoutMs) {
    expire();
    if (result_.state == PingState::Pending) return false;
    if (++sequence_ == 0) ++sequence_;
    char sequence[11];
    std::snprintf(sequence, sizeof(sequence), "%lu", static_cast<unsigned long>(sequence_));
    result_.sequence = sequence;
    result_.state = PingState::Pending;
    startedAt_ = monotonicMilliseconds();
    timeoutMs_ = timeoutMs;
    if (!transport.writeLine(Codec::buildRequest(Command::Ping,
        {{"side", localSide()}, {"seq", result_.sequence}}))) {
      result_.state = PingState::WriteError;
      return false;
    }
    return true;
  }

  void cancel() { result_.state = PingState::Idle; }

  void expire() {
    if (result_.state == PingState::Pending &&
        monotonicMilliseconds() - startedAt_ >= timeoutMs_) {
      result_.state = PingState::Timeout;
    }
  }

  PingResult result() const {
    PingResult value = result_;
    if (value.state == PingState::Pending &&
        monotonicMilliseconds() - startedAt_ >= timeoutMs_) value.state = PingState::Timeout;
    return value;
  }

  bool consume(Transport& transport, const String& message) {
    Request request;
    String error;
    if (!Codec::parseRequest(message, request, error) || request.command != Command::Ping) return false;
    expire();
    // Invalid PING frames are consumed silently; never answer a response.
    if (request.value("side") != remoteSide() || !validSequence(request.value("seq"))) return true;
    if (!request.has("status")) {
      Response response = Response::ok();
      response.parameters = {{"type", "PING"}, {"side", localSide()}, {"seq", request.value("seq")}};
      transport.writeLine(Codec::buildResponse(response));
    } else if (request.value("status") == "1" && result_.state == PingState::Pending &&
               request.value("seq") == result_.sequence) {
      result_.state = PingState::Ok;
    }
    return true;
  }

private:
  static bool validSequence(const String& sequence) {
    const size_t length = stringLength(sequence);
    if (length == 0 || length > 10 || stringCharacter(sequence, 0) == '0') return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
      const char digit = stringCharacter(sequence, i);
      if (digit < '0' || digit > '9') return false;
      const uint32_t number = static_cast<uint32_t>(digit - '0');
      if (value > (UINT32_MAX - number) / 10) return false;
      value = value * 10 + number;
    }
    return true;
  }
  const char* localSide() const { return server_ ? "server" : "client"; }
  const char* remoteSide() const { return server_ ? "client" : "server"; }
  bool server_;
  uint32_t sequence_ = 0;
  unsigned long startedAt_ = 0;
  unsigned long timeoutMs_ = 0;
  PingResult result_;
};

}  // namespace detail
}  // namespace vscp
