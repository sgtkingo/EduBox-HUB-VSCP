#include "ble_transport.hpp"
#include "vscp_client.hpp"
#include "vscp_server.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace edubox::ble;

int main() {
  Channel central, peripheral;
  assert(central.open() && peripheral.open());
  Transport outgoing(central), incoming(peripheral);
  vscp::Client client(outgoing, 1000); client.setSequenceEnabled(true);
  vscp::Server server; server.addTransport(incoming);
  std::atomic<bool> run{true}, holdResponse{false};
  std::atomic<int> controls{0};
  server.on(vscp::Command::Init, [](const vscp::Request&) { return vscp::Response::ok(); });
  server.on(vscp::Command::Connect, [](const vscp::Request&) { return vscp::Response::ok(); });
  server.on(vscp::Command::Control, [&](const vscp::Request&) { ++controls; return vscp::Response::ok(); });
  server.on(vscp::Command::Update, [](const vscp::Request&) { return vscp::Response::ok(); });
  auto pump = [](Channel& source, Channel& destination) {
    Packet packet;
    if (source.packet(23, packet)) {
      destination.receive(packet.data.data(), packet.size, destination.generation(), 10);
      source.confirm(packet);
    }
  };
  std::thread worker([&] {
    while (run) {
      pump(central, peripheral); server.poll();
      if (!holdResponse) pump(peripheral, central);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  assert(client.init("Panel", "1").status == vscp::Status::Ok);
  assert(client.connect("A02", "15").status == vscp::Status::Ok);
  assert(client.control("A02", {{"state", "1"}, {"speed", "50"}}).status == vscp::Status::Ok);
  assert(controls == 1);
  holdResponse = true;
  client.setTimeout(500); // Windows scheduler may round short sleeps to 15 ms.
  assert(client.control("A02", {{"state", "0"}}).error == "Response timeout");
  assert(controls == 2); // Lost response DOES NOT retransmit CONTROL.
  holdResponse = false;
  client.setTimeout(1000);
  auto response = client.update("A02");
  assert(response.status == vscp::Status::Ok && response.parameters.at("seq") == "5");
  assert(controls == 2); // Late same-UID seq=4 ignored.
  central.disconnect();
  const auto started = std::chrono::steady_clock::now();
  assert(client.update("A02").error == "Peer disconnected");
  assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(20));
  assert(client.sessionClosed() && !client.isInitialized());
  assert(central.takeLoss() && central.open());
  assert(client.control("A02", {}).error == "Protocol not initialized");
  assert(controls == 2); // Physical reconnect alone never replays commands.
  run = false; worker.join();
  std::cout << "PASS BLE VSCP integration, late seq, disconnect and no replay\n";
}
