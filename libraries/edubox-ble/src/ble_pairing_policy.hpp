#pragma once
#include <cstdint>
#include <cstring>

namespace edubox { namespace ble {
// PIN 0 denotes bonded-only reconnect, never an automatic pairing credential.
inline bool validCommissioningPin(uint32_t pin) { return pin >= 100000 && pin <= 999999; }
inline bool mayEnterPasskey(uint32_t pin, uint32_t activeEpoch, uint32_t currentEpoch) {
  return validCommissioningPin(pin) && activeEpoch && activeEpoch == currentEpoch;
}
inline bool acceptsPeerIdentity(uint32_t pin, const char* requested, uint8_t requestedType,
                                const char* authenticated, uint8_t authenticatedType) {
  return validCommissioningPin(pin) ||
      (!pin && requestedType == authenticatedType && std::strcmp(requested, authenticated) == 0);
}
}}
