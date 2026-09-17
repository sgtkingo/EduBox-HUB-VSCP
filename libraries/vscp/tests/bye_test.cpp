#include "vscp.hpp"
#include <cassert>
#include <deque>
#include <functional>
#include <vector>

using namespace vscp;

class Wire : public Transport {
public:
  std::deque<String> incoming;
  std::vector<String> outgoing;
  std::function<void(const String&)> onWrite;
  std::function<void()> onRead;
  bool writable = true;
protected:
  bool writeLineImpl(const String& message) override {
    if (!writable) return false;
    outgoing.push_back(message);
    if (onWrite) onWrite(message);
    return true;
  }
  ReadStatus readLineImpl(String& message) override {
    if (onRead) onRead();
    if (incoming.empty()) return ReadStatus::NoData;
    message = incoming.front(); incoming.pop_front();
    return ReadStatus::Message;
  }
};

int main() {
  assert(commandFromName("bye") == Command::Bye);
  assert(String(commandName(Command::Bye)) == "BYE");
  Wire clientWire, serverWire, other, unknown;
  clientWire.onWrite = [&](const String& message) { serverWire.incoming.push_back(message); };
  serverWire.onWrite = [&](const String& message) { clientWire.incoming.push_back(message); };
  Server server;
  server.addTransport(serverWire); server.addTransport(other);
  Client client(clientWire, 100);
  clientWire.onRead = [&] { server.poll(); };
  int byeEvents = 0;
  server.onBye([&](Transport& transport) { assert(&transport == &serverWire); ++byeEvents; });
  server.on(Command::Init, [](const Request& request) {
    assert(request.value("api") == API_VERSION);
    return Response::ok();
  });
  server.on(Command::Update, [](const Request&) { return Response::ok(); });

  // Notification before INIT, no acknowledgement; duplicate is idempotent.
  assert(client.bye()); server.poll();
  assert(client.sessionClosed() && !client.isInitialized());
  assert(byeEvents == 1 && clientWire.incoming.empty());
  assert(client.bye()); server.poll();
  assert(byeEvents == 1 && clientWire.incoming.empty());
  assert(client.init().status == Status::Ok);
  assert(!client.sessionClosed());

  // Closing one endpoint leaves the other initialized and usable.
  other.incoming.push_back(Codec::buildRequest(Command::Init, {{"api", API_VERSION}}));
  server.poll();
  assert(server.ping(serverWire, 100));
  assert(client.bye()); server.poll();
  assert(server.pingResult(serverWire).state == PingState::Idle);
  assert(byeEvents == 2);
  serverWire.incoming.push_back("?type=UPDATE&id=S01");
  other.incoming.push_back("?type=UPDATE&id=S02");
  server.poll();
  ResponseStatus response; String error;
  assert(Codec::parseResponse(clientWire.incoming.back(), response, error));
  assert(response.status == Status::Error && response.error == "Protocol not initialized");
  assert(Codec::parseResponse(other.outgoing.back(), response, error));
  assert(response.status == Status::Ok);
  clientWire.incoming.clear();

  // Server notification during an ordinary transaction interrupts immediately.
  assert(client.init().status == Status::Ok);
  server.on(Command::Update, [&](const Request&) {
    assert(server.bye(serverWire));
    return Response::ok(); // No extra ordinary response after BYE.
  });
  assert(client.update("S01").error == "Peer disconnected");
  assert(client.sessionClosed() && !client.isInitialized());
  assert(clientWire.incoming.empty());
  assert(!server.bye(unknown));
  assert(client.init().status == Status::Ok);

  // Wrong role / acknowledgement-shaped BYE cannot close either endpoint.
  serverWire.incoming.push_back("?type=BYE&side=server");
  serverWire.incoming.push_back("?type=BYE&side=client&status=1");
  server.poll(); server.poll();
  assert(byeEvents == 2);
  clientWire.incoming.push_back("?type=BYE&side=client");
  clientWire.incoming.push_back("?type=BYE&side=server&status=1");
  client.poll(); client.poll();
  assert(client.isInitialized());

  // Incoming BYE also interrupts PING without waiting for timeout.
  serverWire.onWrite = [&](const String&) {
    clientWire.incoming.push_back("?type=BYE&side=server");
  };
  assert(client.ping().error == "Peer disconnected");
  assert(client.sessionClosed());

  // Failed local notification does not pretend the session was closed.
  Wire failing;
  Client failingClient(failing, 100);
  failing.writable = false;
  assert(!failingClient.bye() && !failingClient.sessionClosed());
  serverWire.writable = false;
  assert(!server.bye(serverWire));
}
