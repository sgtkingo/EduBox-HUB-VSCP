/**
 * @file vscp_server.hpp
 * @brief Non-blocking handler-based VSCP responder.
 */

#pragma once

#include "io/vscp_transport.hpp"
#include "vscp_codec.hpp"
#include "vscp_ping.hpp"

#include <functional>
#include <map>
#include <vector>

namespace vscp {

class Server {
public:
  using Handler = std::function<Response(const Request&)>;

  void addTransport(Transport& transport);
  void on(Command command, Handler handler);
  void poll();
  // Non-blocking; poll() routes acknowledgements and answers peer PINGs.
  bool ping(Transport& transport, unsigned long timeoutMs = DEFAULT_TIMEOUT_MS);
  PingResult pingResult(const Transport& transport) const;

private:
  struct Endpoint {
    explicit Endpoint(Transport& endpointTransport) : transport(&endpointTransport) {}
    Transport* transport;
    bool initialized = false;
    detail::PingExchange ping{true};
  };

  Response dispatch(Endpoint& endpoint, const Request& request);
  void process(Endpoint& endpoint, const String& message);

  std::vector<Endpoint> endpoints_;
  std::map<Command, Handler> handlers_;
};

}  // namespace vscp
