#include "vscp.hpp"
#include <cassert>
#include <deque>
#include <vector>

class Wire : public vscp::Transport {
public:
  std::deque<vscp::String> incoming;
  std::vector<vscp::String> outgoing;
protected:
  vscp::ReadStatus readLineImpl(vscp::String& line) override {
    if (incoming.empty()) return vscp::ReadStatus::NoData;
    line = incoming.front(); incoming.pop_front(); return vscp::ReadStatus::Message;
  }
  bool writeLineImpl(const vscp::String& line) override { outgoing.push_back(line); return true; }
};

int main() {
  vscp::Server server;
  Wire first, second, unknown;
  server.addTransport(first); server.addTransport(second);
  vscp::Transport* source = nullptr;
  server.on(vscp::Command::Init, [&source](const vscp::Request&, vscp::Transport& transport) {
    source = &transport; return vscp::Response::ok();
  });
  // Old single-argument handler overload must remain source-compatible.
  server.on(vscp::Command::Update, [](const vscp::Request&) { return vscp::Response::ok(); });
  first.incoming.push_back("?type=INIT&api=1.6");
  second.incoming.push_back("?type=INIT&api=1.6");
  server.poll(); assert(source == &second);
  int callbacks = 0;
  server.onBye([&callbacks](vscp::Transport&) { ++callbacks; });
  assert(!server.closeSession(unknown));
  const auto writes = first.outgoing.size();
  assert(server.ping(first, 1000));
  assert(server.closeSession(first));
  assert(server.pingResult(first).state == vscp::PingState::Idle);
  assert(first.outgoing.size() == writes + 1 && callbacks == 0);
  first.incoming.push_back("?type=UPDATE&id=S01");
  second.incoming.push_back("?type=UPDATE&id=S01");
  server.poll();
  vscp::ResponseStatus response; vscp::String error;
  assert(vscp::Codec::parseResponse(first.outgoing.back(), response, error));
  assert(response.status == vscp::Status::Error);
  assert(vscp::Codec::parseResponse(second.outgoing.back(), response, error));
  assert(response.status == vscp::Status::Ok);
  first.incoming.push_back("?type=INIT&api=1.6"); server.poll();
  assert(source == &first);
  first.incoming.push_back("?type=UPDATE&id=S01"); server.poll();
  assert(vscp::Codec::parseResponse(first.outgoing.back(), response, error));
  assert(response.status == vscp::Status::Ok);
}
