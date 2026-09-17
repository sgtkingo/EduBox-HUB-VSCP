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
  using ContextHandler = std::function<Response(const Request&, Transport&)>;
  using ByeHandler = std::function<void(Transport&)>;

  void addTransport(Transport& transport);
  void on(Command command, Handler handler);
  void on(Command command, ContextHandler handler);
  // Invalidate only this endpoint without sending anything (physical link loss).
  // Does not invoke onBye; application cleanup is the caller's responsibility.
  bool closeSession(Transport& transport);
  void poll();
  bool bye(Transport& transport);
  // Called once when a peer closes its session, with the affected transport.
  void onBye(ByeHandler handler) { byeHandler_ = std::move(handler); }
  // Non-blocking; poll() routes acknowledgements and answers peer PINGs.
  bool ping(Transport& transport, unsigned long timeoutMs = DEFAULT_TIMEOUT_MS);
  PingResult pingResult(const Transport& transport) const;

private:
  struct Endpoint {
    explicit Endpoint(Transport& endpointTransport) : transport(&endpointTransport) {}
    Transport* transport;
    bool initialized = false;
    bool closed = false;
    detail::PingExchange ping{true};
  };

  Response dispatch(Endpoint& endpoint, const Request& request);
  void process(Endpoint& endpoint, const String& message);

  ByeHandler byeHandler_;
  std::vector<Endpoint> endpoints_;
  std::map<Command, ContextHandler> handlers_;
};

}  // namespace vscp
