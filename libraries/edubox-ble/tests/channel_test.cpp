#include "ble_channel.hpp"
#include "ble_transport.hpp"
#include <cassert>
#include <iostream>
#include <string>
using namespace edubox::ble;
void transfer(Channel& source, Channel& target, uint16_t mtu) {
  Packet packet;
  while (source.packet(mtu, packet)) {
    assert(packet.size <= size_t(mtu - 3));
    assert(target.receive(packet.data.data(), packet.size, target.generation(), 10));
    assert(source.confirm(packet));
  }
}
int main() {
  for (auto mtu : {23, 64, 247, 517}) {
    Channel a, b; assert(a.open() && b.open());
    std::string maximum(MaxLine, 'x');
    assert(a.enqueue(maximum.c_str(), maximum.size())); transfer(a, b, mtu);
    Line line; assert(b.read(line) && line.size == MaxLine && maximum == line.data.data());
    assert(!b.read(line));
    const char* ping = "?type=PING&side=server&seq=1";
    assert(b.enqueue(ping, std::strlen(ping)));
    transfer(b, a, mtu); assert(a.read(line));
    assert(a.stats().txLines == 1 && a.stats().rxLines == 1);
  }
  Channel a, b; a.open(); b.open();
  assert(a.enqueue("abcdefghijklmnopqrst", 20));
  Packet old; assert(a.packet(23, old));
  assert(b.receive(old.data.data(), old.size, b.generation(), UINT32_MAX - 1000));
  assert(!b.receive(old.data.data(), old.size, b.generation(), 10)); // Duplicate offset fails closed.
  assert(!b.online() && b.takeLoss());
  b.open();
  assert(b.receive(old.data.data(), old.size, b.generation(), UINT32_MAX - 1000));
  b.expire(1000); assert(!b.online() && b.takeLoss()); // Wrap-safe partial timeout.
  a.disconnect(); assert(!a.open()); assert(a.takeLoss()); assert(a.open());
  assert(!a.confirm(old)); // Generation invalidates stale TX ack.
  assert(!a.receive(old.data.data(), old.size, old.generation, 10)); // Old callback.
  Line line; assert(!a.read(line)); Packet empty; assert(!a.packet(23, empty));
  for (size_t i = 0; i < QueueDepth; ++i) assert(a.enqueue("x", 1));
  assert(!a.enqueue("x", 1) && !a.online() && a.stats().faults == 1);
  a.takeLoss(); a.open(); assert(!a.enqueue("bad\n", 4) && !a.online());
  Channel c, d; c.open(); d.open();
  for (size_t i = 0; i < QueueDepth; ++i) { c.enqueue("x", 1); transfer(c, d, 23); }
  c.enqueue("x", 1); Packet overflow; assert(c.packet(23, overflow));
  assert(!d.receive(overflow.data.data(), overflow.size, d.generation(), 10) && !d.online());
  c.disconnect(); c.takeLoss(); c.open();
  Transport transport(c); assert(transport.writeLine("?status=1"));
  Channel e; e.open(); transfer(c, e, 247);
  Transport receiver(e); vscp::String value;
  assert(receiver.readLine(value) == vscp::ReadStatus::Message && value == "?status=1");
  std::cout << "PASS bounded BLE framing\n";
}
