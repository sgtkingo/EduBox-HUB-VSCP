/**
 * @file vscp_server.cpp
 * @brief Implementation of the non-blocking VSCP responder.
 */

#include "vscp_server.hpp"

namespace vscp {

void Server::addTransport(Transport& transport) {
  endpoints_.emplace_back(transport);
}

void Server::on(Command command, Handler handler) {
  handlers_[command] = [handler](const Request& request, Transport&) { return handler(request); };
}

void Server::on(Command command, ContextHandler handler) {
  handlers_[command] = std::move(handler);
}

bool Server::closeSession(Transport& transport) {
  for (auto& endpoint : endpoints_) {
    if (endpoint.transport != &transport) continue;
    endpoint.initialized = false;
    endpoint.closed = true;
    endpoint.ping.cancel();
    return true;
  }
  return false;
}

Response Server::dispatch(Endpoint& endpoint, const Request& request) {
  if (request.command != Command::Init && !endpoint.initialized) {
    return Response::fail("Protocol not initialized");
  }

  const auto handler = handlers_.find(request.command);
  if (request.command == Command::Unknown || handler == handlers_.end()) {
    return Response::fail("Unknown type");
  }

  Response response = handler->second(request, *endpoint.transport);
  if (request.command == Command::Init) {
    endpoint.initialized = response.status == Status::Ok;
    if (endpoint.initialized) endpoint.closed = false;
  }
  return response;
}

void Server::process(Endpoint& endpoint, const String& message) {
  if (endpoint.ping.consume(*endpoint.transport, message)) return;
  Request request;
  String parseError;
  if (!Codec::parseRequest(message, request, parseError)) {
    endpoint.transport->writeLine(Codec::buildResponse(Response::fail(parseError)));
    return;
  }

  if (request.command == Command::Bye) {
    if (request.value("side") == "client" && !request.has("status")) {
      const bool notify = !endpoint.closed;
      endpoint.initialized = false;
      endpoint.closed = true;
      endpoint.ping.cancel();
      if (notify && byeHandler_) byeHandler_(*endpoint.transport);
    }
    return; // BYE is a notification, including before INIT. Never acknowledge it.
  }

  const bool wasClosed = endpoint.closed;
  Response response = dispatch(endpoint, request);
  if (!wasClosed && endpoint.closed) return; // Handler sent BYE instead of an ordinary response.
  const String requestId = request.value("id");
  // Optional ordinary transaction correlation; no change for legacy requests.
  if (request.has("seq")) response.parameters["seq"] = request.value("seq");
  if (requestId.length() > 0 && response.parameters.find("id") == response.parameters.end()) {
    response.parameters["id"] = requestId;
  }
  endpoint.transport->writeLine(Codec::buildResponse(response));
}

bool Server::bye(Transport& transport) {
  for (auto& endpoint : endpoints_) {
    if (endpoint.transport != &transport) continue;
    if (!transport.writeLine(Codec::buildRequest(Command::Bye, {{"side", "server"}}))) return false;
    endpoint.initialized = false;
    endpoint.closed = true;
    endpoint.ping.cancel();
    return true;
  }
  return false;
}

bool Server::ping(Transport& transport, unsigned long timeoutMs) {
  for (auto& endpoint : endpoints_) {
    if (endpoint.transport == &transport) return endpoint.ping.start(transport, timeoutMs);
  }
  return false;
}

PingResult Server::pingResult(const Transport& transport) const {
  for (const auto& endpoint : endpoints_) {
    if (endpoint.transport == &transport) return endpoint.ping.result();
  }
  return PingResult();
}

void Server::poll() {
  for (auto& endpoint : endpoints_) {
    endpoint.ping.expire();
    String message;
    const ReadStatus readStatus = endpoint.transport->readLine(message);
    if (readStatus == ReadStatus::MessageTooLong) {
      endpoint.transport->writeLine(Codec::buildResponse(Response::fail("Request too long")));
    } else if (readStatus == ReadStatus::Message) {
      process(endpoint, message);
    }
  }
}

}  // namespace vscp
