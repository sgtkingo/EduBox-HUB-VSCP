/**
 * @file vscp_client.cpp
 * @brief Implementation of the transport-injected VSCP request client.
 */

#include "vscp_client.hpp"
#include <cstdio>

namespace vscp {

Client::Client(Transport& transport, unsigned long timeoutMs)
    : transport_(transport), timeoutMs_(timeoutMs) {}

String Client::nextSequence() {
  if (++sequence_ == 0) ++sequence_;
  char buffer[11];
  std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(sequence_));
  return String(buffer);
}

ResponseStatus Client::transact(Command command, Parameters parameters, unsigned long timeoutOverrideMs) {
  ResponseStatus result;
  if (transacting_) { result.error = "Transaction already pending"; return result; }
  transacting_ = true;
  struct Guard { bool& busy; ~Guard() { busy = false; } } guard{transacting_};
  if (!transport_.isAvailable()) { closeSession(); result.error = "Peer disconnected"; return result; }
  const auto expectedIdEntry = parameters.find("id");
  const String expectedId = expectedIdEntry == parameters.end() ? String() : expectedIdEntry->second;

  String expectedSequence;
  if (sequenceEnabled_) {
    expectedSequence = nextSequence();
    parameters["seq"] = expectedSequence;
  }
  if (!transport_.writeLine(Codec::buildRequest(command, parameters))) {
    result.error = "Request write failed";
    return result;
  }
  const unsigned long transactionTimeoutMs = timeoutOverrideMs ? timeoutOverrideMs : timeoutMs_;
  const unsigned long startedAt = detail::monotonicMilliseconds();
  while (detail::monotonicMilliseconds() - startedAt < transactionTimeoutMs) {
    if (!transport_.isAvailable()) { closeSession(); result.error = "Peer disconnected"; return result; }
    String message;
    const ReadStatus readStatus = transport_.readLine(message);
    if (readStatus == ReadStatus::NoData) {
      detail::sleepMilliseconds(1);
      continue;
    }
    if (readStatus == ReadStatus::MessageTooLong) {
      result.error = "Response too long";
      return result;
    }

    if (consumeBye(message)) { result.error = "Peer disconnected"; return result; }
    if (ping_.consume(transport_, message)) continue;

    String parseError;
    if (!Codec::parseResponse(message, result, parseError)) {
      result.error = parseError;
      return result;
    }
    if (sequenceEnabled_) {
      const auto seq = result.parameters.find("seq");
      if (seq == result.parameters.end() || seq->second != expectedSequence) {
        result = ResponseStatus(); // Late/unsequenced response is not this transaction.
        continue;
      }
    }
    if (detail::stringLength(expectedId) > 0) {
      const auto responseId = result.parameters.find("id");
      if (responseId == result.parameters.end() || responseId->second != expectedId) {
        result.status = Status::Error;
        result.error = "Response UID mismatch";
      }
    }
    return result;
  }

  result.error = "Response timeout";
  return result;
}

void Client::closeSession() {
  initialized_ = false;
  closed_ = true;
  ping_.cancel();
}

bool Client::consumeBye(const String& message) {
  Request request;
  String error;
  if (!Codec::parseRequest(message, request, error) || request.command != Command::Bye || request.has("status")) return false;
  initialized_ = false;
  closed_ = true;
  ping_.cancel();
  return true;
}

bool Client::bye(bool waitForResponse, unsigned long responseTimeoutMs) {
  if (transacting_) return false;
  if (!waitForResponse) {
    if (!transport_.writeLine(Codec::buildRequest(Command::Bye, {}))) return false;
    closeSession();
    return true;
  }
  const auto response = transact(Command::Bye, {}, responseTimeoutMs);
  closeSession();
  return response.status == Status::Ok;
}

void Client::poll() {
  if (transacting_) return;
  if (!transport_.isAvailable()) { closeSession(); return; }
  ping_.expire();
  String message;
  if (transport_.readLine(message) == ReadStatus::Message && !consumeBye(message))
    ping_.consume(transport_, message);
}

ResponseStatus Client::ping() {
  ResponseStatus response;
  if (transacting_) { response.error = "Transaction already pending"; return response; }
  transacting_ = true;
  struct Guard { bool& busy; ~Guard() { busy = false; } } guard{transacting_};
  if (!ping_.start(transport_, timeoutMs_, nextSequence())) {
    response.error = "PING write failed";
    return response;
  }
  while (ping_.result().state == PingState::Pending) {
    if (!transport_.isAvailable()) { closeSession(); response.error = "Peer disconnected"; return response; }
    String message;
    const ReadStatus status = transport_.readLine(message);
    if (status == ReadStatus::Message) {
      if (consumeBye(message)) { response.error = "Peer disconnected"; return response; }
      ping_.consume(transport_, message);
    }
    else if (status == ReadStatus::MessageTooLong) { ping_.cancel(); response.error = "Response too long"; return response; }
    else detail::sleepMilliseconds(1);
  }
  const PingResult result = ping_.result();
  if (result.state == PingState::Ok) {
    response.status = Status::Ok;
    response.parameters = {{"seq", result.sequence}, {"status", "1"}};
  } else response.error = "Response timeout";
  return response;
}

ResponseStatus Client::init(const String& application, const String& databaseVersion) {
  Parameters parameters;
  parameters["api"] = API_VERSION;
  if (detail::stringLength(application) > 0) parameters["app"] = application;
  if (detail::stringLength(databaseVersion) > 0) parameters["db"] = databaseVersion;
  ResponseStatus response = transact(Command::Init, parameters);
  initialized_ = response.status == Status::Ok;
  if (initialized_) closed_ = false;
  return response;
}

ResponseStatus Client::connect(const String& uid, const String& pins) {
  Parameters parameters{{"id", uid}, {"pins", pins}};
  return transact(Command::Connect, parameters);
}

ResponseStatus Client::disconnect(const String& uid) {
  return transact(Command::Disconnect, Parameters{{"id", uid}});
}

ResponseStatus Client::update(const String& uid) {
  return transact(Command::Update, Parameters{{"id", uid}});
}

ResponseStatus Client::config(const String& uid, const Parameters& parameters) {
  Parameters requestParameters = parameters;
  requestParameters["id"] = uid;
  return transact(Command::Config, requestParameters);
}

ResponseStatus Client::control(const String& uid, const Parameters& parameters) {
  Parameters requestParameters = parameters;
  requestParameters["id"] = uid;
  return transact(Command::Control, requestParameters);
}

ResponseStatus Client::reset(const String& uid) {
  return transact(Command::Reset, Parameters{{"id", uid}});
}

}  // namespace vscp
