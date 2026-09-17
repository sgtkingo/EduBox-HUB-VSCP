#include "vscp.hpp"
#include <cassert>
#include <deque>
#include <functional>
#include <vector>

using namespace vscp;

class TestTransport : public Transport {
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
    message = incoming.front();
    incoming.pop_front();
    return ReadStatus::Message;
  }
};

Request parse(const String& message) {
  Request request;
  String error;
  assert(Codec::parseRequest(message, request, error));
  return request;
}

int main() {
  assert(commandFromName("ping") == Command::Ping);
  assert(String(commandName(Command::Ping)) == "PING");
  TestTransport clientWire, serverWire, otherWire, unknownWire;
  clientWire.onWrite = [&](const String& message) { serverWire.incoming.push_back(message); };
  serverWire.onWrite = [&](const String& message) { clientWire.incoming.push_back(message); };
  Server server;
  server.addTransport(serverWire);
  server.addTransport(otherWire);
  Client client(clientWire, 20);
  clientWire.onRead = [&] { server.poll(); };

  // Both directions work before INIT, without handlers or state changes.
  assert(client.ping().status == Status::Ok);
  assert(!client.isInitialized());
  assert(server.ping(serverWire, 20));
  assert(!server.ping(serverWire, 20));
  client.poll(); server.poll();
  assert(server.pingResult(serverWire).state == PingState::Ok);
  assert(server.pingResult(otherWire).state == PingState::Idle);
  assert(!server.ping(unknownWire));
  const size_t responseCount = clientWire.outgoing.size();
  client.poll(); server.poll();
  assert(clientWire.outgoing.size() == responseCount); // No response loop.

  // Simultaneous PING with overlapping sequence spaces.
  assert(server.ping(serverWire, 20));
  assert(client.ping().status == Status::Ok);
  server.poll();
  assert(server.pingResult(serverWire).state == PingState::Ok);

  server.on(Command::Init, [](const Request&) { return Response::ok(); });
  assert(client.init().status == Status::Ok);
  server.on(Command::Update, [&](const Request&) {
    assert(server.ping(serverWire, 20));
    Response response = Response::ok();
    response.parameters["value"] = "123";
    return response;
  });
  const auto update = client.update("S01");
  assert(update.status == Status::Ok && update.parameters.at("value") == "123");
  server.poll();
  assert(server.pingResult(serverWire).state == PingState::Ok);

  // Wrong sequence, side, error status and malformed sequence cannot acknowledge.
  assert(server.ping(otherWire, 20));
  const String seq = server.pingResult(otherWire).sequence;
  for (const String& message : std::vector<String>{
      "?type=PING&side=client&seq=999&status=1",
      "?type=PING&side=server&seq=" + seq + "&status=1",
      "?type=PING&side=client&seq=" + seq + "&status=0",
      "?type=PING&side=client&seq=4294967296&status=1",
      "?type=PING&side=client&seq=0", "?type=PING&side=client&seq=01",
      "?type=PING&side=client", "?type=PING&seq=1"}) {
    otherWire.incoming.push_back(message);
    const auto count = otherWire.outgoing.size();
    server.poll();
    assert(server.pingResult(otherWire).state == PingState::Pending);
    assert(otherWire.outgoing.size() == count);
  }
  otherWire.incoming.push_back("?type=PING&side=client&seq=" + seq + "&status=1");
  server.poll();
  assert(server.pingResult(otherWire).state == PingState::Ok);

  // Timeout, late/duplicate response, and a fresh exchange.
  assert(server.ping(otherWire, 1));
  const String oldSeq = server.pingResult(otherWire).sequence;
  detail::sleepMilliseconds(3);
  assert(server.pingResult(otherWire).state == PingState::Timeout);
  assert(server.ping(otherWire, 20));
  const String newSeq = server.pingResult(otherWire).sequence;
  assert(oldSeq != newSeq);
  otherWire.incoming.push_back("?type=PING&side=client&seq=" + oldSeq + "&status=1");
  server.poll();
  assert(server.pingResult(otherWire).state == PingState::Pending);
  otherWire.incoming.push_back("?type=PING&side=client&seq=" + newSeq + "&status=1");
  server.poll();
  assert(server.pingResult(otherWire).state == PingState::Ok);
  otherWire.incoming.push_back("?type=PING&side=client&seq=" + newSeq + "&status=1");
  const auto count = otherWire.outgoing.size();
  server.poll();
  assert(otherWire.outgoing.size() == count);

  // Client filters mismatched and untagged acknowledgements too.
  TestTransport scripted;
  Client scriptedClient(scripted, 10);
  scripted.onWrite = [&](const String& message) {
    const Request request = parse(message);
    if (request.command != Command::Ping || request.has("status")) return;
    scripted.incoming.push_back("?status=1");
    scripted.incoming.push_back("?type=PING&side=server&seq=999&status=1");
    scripted.incoming.push_back("?type=PING&side=client&seq=" + request.value("seq") + "&status=1");
    scripted.incoming.push_back("?type=PING&side=server&seq=" + request.value("seq") + "&status=1");
  };
  assert(scriptedClient.ping().status == Status::Ok);
  scripted.onWrite = [&](const String& message) {
    if (parse(message).command != Command::Init) return;
    scripted.incoming.push_back("?type=PING&side=server&seq=1&status=1");
    scripted.incoming.push_back("?type=PING&side=server&seq=77");
    scripted.incoming.push_back("?status=1&api=" + String(API_VERSION));
  };
  const auto init = scriptedClient.init();
  assert(init.status == Status::Ok && init.parameters.at("api") == API_VERSION);
  assert(init.parameters.count("type") == 0);
  const auto answered = parse(scripted.outgoing.back());
  assert(answered.value("seq") == "77" && answered.value("side") == "client");
  assert(answered.value("status") == "1");
  scripted.onWrite = nullptr;
  assert(scriptedClient.ping().error == "Response timeout");
  scripted.writable = false;
  assert(scriptedClient.ping().error == "PING write failed");
  otherWire.writable = false;
  assert(!server.ping(otherWire));
  assert(server.pingResult(otherWire).state == PingState::WriteError);
}
