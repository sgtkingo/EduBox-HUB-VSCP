/**
 * @file vscp_codec.hpp
 * @brief Transport-independent parser and serializer for VSCP messages.
 */

#pragma once

#include "vscp_types.hpp"

namespace vscp {

class Codec {
public:
  static bool parseRequest(const String& message, Request& request, String& error);
  static bool parseResponse(const String& message, ResponseStatus& response, String& error);
  static String buildRequest(Command command, const Parameters& parameters = Parameters());
  static String buildResponse(const Response& response);

  // Parse fields without assuming request or response framing.
  static bool parseParameters(const String& message, Parameters& parameters, String& error);

private:
  static String buildParameters(const Parameters& parameters);
};

}  // namespace vscp
