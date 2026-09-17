/**
 * @file vscp_client.hpp
 * @brief Synchronous VSCP request client for HMI and controller applications.
 */

#pragma once

#include "io/vscp_transport.hpp"
#include "vscp_codec.hpp"
#include "vscp_ping.hpp"

namespace vscp {

class Client {
public:
  explicit Client(Transport& transport, unsigned long timeoutMs = DEFAULT_TIMEOUT_MS);

  // Call poll regularly while idle to answer server-initiated PING.
  void poll();
  ResponseStatus ping();
  // One-way session close notification. No response is expected.
  bool bye();
  bool sessionClosed() const { return closed_; }
  // Physical link loss: no write/BYE, cancel pending protocol state locally.
  void closeSession();
  // Opt-in API 1.6 revision: require matching echoed seq on ordinary responses.
  // Enable before INIT only against a server that supports seq echo.
  void setSequenceEnabled(bool enabled) { sequenceEnabled_ = enabled; }

  ResponseStatus init(const String& application = "", const String& databaseVersion = "");
  ResponseStatus connect(const String& uid, const String& pins);
  ResponseStatus disconnect(const String& uid);
  ResponseStatus update(const String& uid);
  ResponseStatus config(const String& uid, const Parameters& parameters);
  ResponseStatus control(const String& uid, const Parameters& parameters);
  ResponseStatus reset(const String& uid);

  bool isInitialized() const { return initialized_; }
  const char* apiVersion() const { return API_VERSION; }

private:
  bool consumeBye(const String& message);
  ResponseStatus transact(Command command, Parameters parameters, bool requiresInit, const String& expectedId = "");

  Transport& transport_;
  unsigned long timeoutMs_;
  bool initialized_ = false;
  bool closed_ = false;
  bool transacting_ = false;
  bool sequenceEnabled_ = false;
  uint32_t sequence_ = 0;
  detail::PingExchange ping_{false};
};

}  // namespace vscp
