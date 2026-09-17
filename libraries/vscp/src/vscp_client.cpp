/**
 * @file vscp_client.cpp
 * @brief Implementation of the transport-injected VSCP request client.
 */

#include "vscp_client.hpp"

namespace vscp {

Client::Client(Transport& transport, unsigned long timeoutMs)
    : transport_(transport), timeoutMs_(timeoutMs) {}

ResponseStatus Client::transact(Command command, Parameters parameters, bool requiresInit, const String& expectedId) {
  ResponseStatus result;
  if (transacting_) { result.error = "Transaction already pending"; return result; }
  transacting_ = true;
  struct Guard { bool& busy; ~Guard() { busy = false; } } guard{transacting_};
  if (requiresInit && !initialized_) {
    result.error = "Protocol not initialized";
    return result;
  }

  if (!transport_.writeLine(Codec::buildRequest(command, parameters))) {
    result.error = "Request write failed";
    return result;
  }
  const unsigned long startedAt = detail::monotonicMilliseconds();
  while (detail::monotonicMilliseconds() - startedAt < timeoutMs_) {
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

bool Client::consumeBye(const String& message) {
  Request request;
  String error;
  if (!Codec::parseRequest(message, request, error) || request.command != Command::Bye ||
      request.value("side") != "server" || request.has("status")) return false;
  initialized_ = false;
  closed_ = true;
  ping_.cancel();
  return true;
}

bool Client::bye() {
  if (transacting_) return false;
  if (!transport_.writeLine(Codec::buildRequest(Command::Bye, {{"side", "client"}}))) return false;
  initialized_ = false;
  closed_ = true;
  ping_.cancel();
  return true;
}

void Client::poll() {
  if (transacting_) return;
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
  if (!ping_.start(transport_, timeoutMs_)) {
    response.error = "PING write failed";
    return response;
  }
  while (ping_.result().state == PingState::Pending) {
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
    response.parameters = {{"side", "server"}, {"seq", result.sequence}, {"status", "1"}};
  } else response.error = "Response timeout";
  return response;
}

ResponseStatus Client::init(const String& application, const String& databaseVersion) {
  Parameters parameters;
  parameters["api"] = API_VERSION;
  if (detail::stringLength(application) > 0) parameters["app"] = application;
  if (detail::stringLength(databaseVersion) > 0) parameters["db"] = databaseVersion;
  ResponseStatus response = transact(Command::Init, parameters, false);
  initialized_ = response.status == Status::Ok;
  if (initialized_) closed_ = false;
  return response;
}

ResponseStatus Client::connect(const String& uid, const String& pins) {
  Parameters parameters{{"id", uid}, {"pins", pins}};
  return transact(Command::Connect, parameters, true, uid);
}

ResponseStatus Client::disconnect(const String& uid) {
  return transact(Command::Disconnect, Parameters{{"id", uid}}, true, uid);
}

ResponseStatus Client::update(const String& uid) {
  return transact(Command::Update, Parameters{{"id", uid}}, true, uid);
}

ResponseStatus Client::config(const String& uid, const Parameters& parameters) {
  Parameters requestParameters = parameters;
  requestParameters["id"] = uid;
  return transact(Command::Config, requestParameters, true, uid);
}

ResponseStatus Client::control(const String& uid, const Parameters& parameters) {
  Parameters requestParameters = parameters;
  requestParameters["id"] = uid;
  return transact(Command::Control, requestParameters, true, uid);
}

ResponseStatus Client::reset(const String& uid) {
  return transact(Command::Reset, Parameters{{"id", uid}}, true, uid);
}

}  // namespace vscp
