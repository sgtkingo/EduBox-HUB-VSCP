#pragma once
#include "ble_channel.hpp"
#include <io/vscp_transport.hpp>

namespace edubox { namespace ble {
class Transport : public vscp::Transport {
  Channel& channel_;
public:
  explicit Transport(Channel& channel) : channel_(channel) {}
  bool isAvailable() const override { return channel_.online(); }
protected:
  vscp::ReadStatus readLineImpl(vscp::String& message) override {
    Line line;
    if (!channel_.read(line)) return vscp::ReadStatus::NoData;
    message = line.data.data(); return vscp::ReadStatus::Message;
  }
  bool writeLineImpl(const vscp::String& message) override {
    return channel_.enqueue(vscp::detail::stringData(message), vscp::detail::stringLength(message));
  }
};
}} // namespace edubox::ble
