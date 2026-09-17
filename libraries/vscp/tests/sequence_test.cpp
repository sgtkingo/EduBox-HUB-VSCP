#include "vscp_client.hpp"
#include "vscp_server.hpp"
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>

class Wire : public vscp::Transport {
public:
  std::deque<vscp::String> rx, tx;
  std::function<void(const vscp::String&)> written;
  bool writable = true;
private:
  vscp::ReadStatus readLineImpl(vscp::String& value) override {
    if (rx.empty()) return vscp::ReadStatus::NoData;
    value = rx.front(); rx.pop_front(); return vscp::ReadStatus::Message;
  }
  bool writeLineImpl(const vscp::String& value) override {
    if (!writable) return false;
    tx.push_back(value); if (written) written(value); return true;
  }
};
int main() {
  Wire wire;
  vscp::Server server;
  server.addTransport(wire);
  server.on(vscp::Command::Init, [](const vscp::Request&) { return vscp::Response::ok(); });
  server.on(vscp::Command::Update, [](const vscp::Request&) { return vscp::Response::ok(); });
  wire.rx.push_back("?type=INIT&api=1.6&seq=37"); server.poll();
  vscp::ResponseStatus response; vscp::String error;
  assert(vscp::Codec::parseResponse(wire.tx.back(), response, error));
  assert(response.parameters.at("seq") == "37");
  wire.rx.push_back("?type=UPDATE&id=S01"); server.poll();
  assert(vscp::Codec::parseResponse(wire.tx.back(), response, error));
  assert(response.parameters.count("seq") == 0); // Legacy request unchanged.
  wire.rx.push_back("?type=BOGUS&seq=38"); server.poll();
  assert(vscp::Codec::parseResponse(wire.tx.back(), response, error));
  assert(response.status == vscp::Status::Error && response.parameters.at("seq") == "38");

  Wire clientWire;
  vscp::Client client(clientWire, 30);
  client.setSequenceEnabled(true);
  clientWire.written = [&](const vscp::String& line) {
    vscp::Request request; assert(vscp::Codec::parseRequest(line, request, error));
    clientWire.rx.push_back("?status=1&id=S01&seq=0"); // Old response.
    clientWire.rx.push_back("?status=1&id=S01");       // Legacy response.
    auto ok = vscp::Response::ok();
    ok.parameters["seq"] = request.value("seq");
    if (request.has("id")) ok.parameters["id"] = request.value("id");
    clientWire.rx.push_back(vscp::Codec::buildResponse(ok));
  };
  assert(client.init().status == vscp::Status::Ok);
  assert(client.update("S01").status == vscp::Status::Ok);
  clientWire.written = nullptr;
  assert(client.update("S01").status == vscp::Status::Error); // Timeout, no replay.
  clientWire.written = [&](const vscp::String& line) {
    vscp::Request request; assert(vscp::Codec::parseRequest(line, request, error));
    clientWire.rx.push_back("?status=1&id=S01&seq=3"); // Timed-out transaction.
    clientWire.rx.push_back("?status=1&id=S01&seq=" + request.value("seq"));
  };
  assert(client.update("S01").parameters.at("seq") == "4");
  const auto writes = clientWire.tx.size();
  client.closeSession();
  assert(!client.isInitialized() && client.sessionClosed() && clientWire.tx.size() == writes);
  assert(client.control("A02", {}).status == vscp::Status::Error);
  assert(clientWire.tx.size() == writes);
  std::cout << "PASS optional seq and local close\n";
}
