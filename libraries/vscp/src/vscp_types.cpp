/**
 * @file vscp_types.cpp
 * @brief Implementation of shared VSCP value types.
 */

#include "vscp_types.hpp"

namespace vscp {

String Request::value(const String& key) const {
  const auto item = parameters.find(key);
  return item == parameters.end() ? String() : item->second;
}

bool Request::has(const String& key) const {
  return parameters.find(key) != parameters.end();
}

Response Response::ok() {
  Response response;
  response.status = Status::Ok;
  return response;
}

Response Response::fail(const String& errorMessage) {
  Response response;
  response.status = Status::Error;
  response.error = errorMessage;
  return response;
}

const char* commandName(Command command) {
  switch (command) {
    case Command::Init: return "INIT";
    case Command::Connect: return "CONNECT";
    case Command::Disconnect: return "DISCONNECT";
    case Command::Update: return "UPDATE";
    case Command::Config: return "CONFIG";
    case Command::Control: return "CONTROL";
    case Command::Reset: return "RESET";
    case Command::Ping: return "PING";
    case Command::Bye: return "BYE";
    case Command::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

Command commandFromName(String name) {
  detail::trimString(name);
  detail::uppercaseString(name);
  if (name == "INIT") return Command::Init;
  if (name == "CONNECT") return Command::Connect;
  if (name == "DISCONNECT") return Command::Disconnect;
  if (name == "UPDATE") return Command::Update;
  if (name == "CONFIG") return Command::Config;
  if (name == "CONTROL") return Command::Control;
  if (name == "RESET") return Command::Reset;
  if (name == "PING") return Command::Ping;
  if (name == "BYE") return Command::Bye;
  return Command::Unknown;
}

}  // namespace vscp
