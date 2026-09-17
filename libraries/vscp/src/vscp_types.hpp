/**
 * @file vscp_types.hpp
 * @brief Shared protocol constants, commands, requests, and responses.
 */

#pragma once

#include "vscp_platform.hpp"

#include <cstdint>
#include <map>

namespace vscp {

constexpr const char* API_VERSION = VSCP_API_VERSION;
constexpr size_t MAX_MESSAGE_SIZE = MAX_PROTOCOL_REQUEST_SIZE;
constexpr unsigned long DEFAULT_TIMEOUT_MS = PROTOCOL_INIT_TIMEOUT;

using Parameters = std::map<String, String>;

enum class Command : uint8_t {
  Unknown,
  Init,
  Connect,
  Disconnect,
  Update,
  Config,
  Control,
  Reset,
  Ping,
  Bye
};

enum class Status : uint8_t {
  Error = 0,
  Ok = 1
};

enum class PingState : uint8_t { Idle, Pending, Ok, Timeout, WriteError };

struct PingResult {
  PingState state = PingState::Idle;
  String sequence;
};

struct Request {
  Command command = Command::Unknown;
  Parameters parameters;

  String value(const String& key) const;
  bool has(const String& key) const;
};

struct Response {
  Status status = Status::Error;
  String error;
  Parameters parameters;

  static Response ok();
  static Response fail(const String& errorMessage);
};

struct ResponseStatus {
  Status status = Status::Error;
  String error;
  Parameters parameters;
};

const char* commandName(Command command);
Command commandFromName(String name);

}  // namespace vscp
