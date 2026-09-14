# EduBox HUB VSCP

**EduBox HUB VSCP** (Virtual Sensors Communication Protocol) is the communication
branch of the [EduBox HUB](https://github.com/sgtkingo/EduBox-HUB) ecosystem.
It connects the HUB with [EduBox HUB App](https://github.com/sgtkingo/EduBox-HUB-App),
[EduBox HUB Panel](https://github.com/sgtkingo/EduBox-HUB-Panel), emulators and
compatible custom hardware.

This repository contains version `2.0.0` of the protocol library. It provides
both sides of the protocol: a synchronous request
`Client` for an HMI/controller and a handler-based, non-blocking `Server` for a
device or HUB. The current wire API version is `1.4`.

This project is not the event-based *Very Simple Control Protocol*.

## Architecture

The library is split into independent layers and does not use a global serial
port:

- `vscp::Codec` parses and serializes protocol frames;
- `vscp::Transport` defines the line-oriented communication interface;
- `vscp::StreamTransport` adapts an Arduino `Stream` such as `Serial1`;
- `vscp::IostreamTransport` and `vscp::StdioTransport` support desktop programs;
- `vscp::Client` sends requests and waits for responses;
- `vscp::Server` dispatches incoming requests to application handlers.

All public components are available through:

```cpp
#include <vscp.hpp>
```

## Wire format

Each message is a line beginning with `?`. Parameters are written as
`key=value` pairs separated by `&`:

```text
?type=UPDATE&id=S01
?id=S01&status=1&temperature=23.5
```

Parameter order is not significant. Command names are parsed without regard to
case; parameter names and values are case-sensitive. The codec does not perform
URL escaping, so values must not contain `&`. Transport input is limited to
printable ASCII and surrounding whitespace is removed.

The default maximum frame length is 1024 characters. Arduino stream frames may
be terminated with LF, CR, or NUL. Desktop transports use newline-delimited
frames.

### Commands

| Command | Client call | Request parameters | Purpose |
| --- | --- | --- | --- |
| `INIT` | `init(app, db)` | `api`, optional `app`, optional `db` | Negotiate compatibility and open a session |
| `CONNECT` | `connect(uid, pins)` | `id`, `pins` | Assign one or more pins, for example `"5"` or `"5,6,7"` |
| `DISCONNECT` | `disconnect(uid)` | `id` | Remove the current pin assignment |
| `UPDATE` | `update(uid)` | `id` | Read the current sensor/device values |
| `CONFIG` | `config(uid, parameters)` | `id` and application parameters | Change persistent or operational configuration |
| `CONTROL` | `control(uid, parameters)` | `id` and application parameters | Send a control value or command |
| `RESET` | `reset(uid)` | `id` | Reset the addressed device |

Every response contains `status=1` for success or `status=0` for failure. A
failure can include `error=...`; other returned fields are command-specific:

```text
?status=1
?id=S01&status=1&temperature=23.5
?error=Device not found&id=S01&status=0
```

## Arduino client

Create a transport for the serial interface used by VSCP, then inject it into a
client. `init()` must succeed before any device command is sent.

```cpp
#include <vscp.hpp>

vscp::StreamTransport transport(Serial1);
vscp::Client client(transport);  // default response timeout: 500 ms

void setup() {
  Serial.begin(115200);   // diagnostics/application console
  Serial1.begin(115200);  // VSCP connection

  vscp::ResponseStatus response = client.init("hmi", "1.3");
  if (response.status != vscp::Status::Ok) {
    Serial.println(response.error);
    return;
  }

  response = client.connect("S01", "5");
  if (response.status != vscp::Status::Ok) {
    Serial.println(response.error);
  }
}

void loop() {
  if (!client.isInitialized()) return;

  const vscp::ResponseStatus response = client.update("S01");
  if (response.status == vscp::Status::Ok) {
    const auto temperature = response.parameters.find("temperature");
    if (temperature != response.parameters.end()) {
      Serial.println(temperature->second);
    }
  } else {
    Serial.println(response.error);
  }

  delay(1000);
}
```

Configuration and control values are supplied with `vscp::Parameters`, which is
a map of `vscp::String` keys and values:

```cpp
client.config("S01", vscp::Parameters{{"sample_rate", "1000"}});
client.control("H00", vscp::Parameters{{"set_point", "35"}});
client.reset("S01");
client.disconnect("S01");
```

The client validates the response format and, for device commands, verifies
that the response contains the requested `id`. Communication and protocol
errors are returned as `Status::Error` with text in `ResponseStatus::error`;
normal protocol operations do not throw exceptions.

## Arduino server

Register one handler for every supported command, add the transport, and call
`poll()` frequently from the main loop:

```cpp
#include <vscp.hpp>

vscp::StreamTransport transport(Serial1);
vscp::Server server;

void setup() {
  Serial1.begin(115200);
  server.addTransport(transport);

  server.on(vscp::Command::Init, [](const vscp::Request& request) {
    if (request.value("api") != vscp::API_VERSION) {
      return vscp::Response::fail("API mismatch");
    }
    return vscp::Response::ok();
  });

  server.on(vscp::Command::Connect, [](const vscp::Request& request) {
    if (!request.has("id")) return vscp::Response::fail("Missing id");
    if (request.value("pins").length() == 0) {
      return vscp::Response::fail("Missing pins");
    }

    // Connect request.value("id") to request.value("pins").
    return vscp::Response::ok();
  });

  server.on(vscp::Command::Update, [](const vscp::Request& request) {
    if (request.value("id") != "S01") {
      return vscp::Response::fail("Device not found");
    }

    vscp::Response response = vscp::Response::ok();
    response.parameters["temperature"] = "23.5";
    return response;
  });
}

void loop() {
  server.poll();
}
```

`poll()` processes at most one complete frame from each registered transport on
each call. The Arduino transport itself is non-blocking. Initialization state is
tracked separately for every transport, and non-`INIT` requests are rejected
until that endpoint completes a successful `INIT` handler. The server
automatically copies a request `id` into the response unless the handler already
provided one.

Handlers receive the complete parsed `vscp::Request`. Use `request.has(key)` to
distinguish a missing parameter from an empty value and `request.value(key)` to
read it. A handler returns either `vscp::Response::ok()` or
`vscp::Response::fail(message)` and may add response parameters.

## Desktop transports

Desktop builds expose C++ stream and C `FILE*` adapters by default:

```cpp
vscp::IostreamTransport cppTransport(std::cin, std::cout);
vscp::StdioTransport cTransport(stdin, stdout);
```

These adapters perform blocking line reads. A desktop server event loop should
therefore run a blocking transport on a dedicated input thread. A client timeout
cannot interrupt a blocking `std::getline()` or `fgets()` call; use a custom
non-blocking transport when a strict timeout is required.

To support another communication channel, derive from `vscp::Transport` and
implement `readLineImpl()` and `writeLineImpl()`. Return `ReadStatus::NoData`,
`ReadStatus::Message`, or `ReadStatus::MessageTooLong` as appropriate.

## Diagnostics

Diagnostics are compile-time controlled by `PROTOCOL_VERBOSE`:

- `0`: disabled;
- `1`: framing and write errors;
- `2`: errors plus complete `[VSCP][RX]` and `[VSCP][TX]` frames.

A log sink must be explicitly attached. Keep diagnostics on a different channel
from the protocol so log text cannot be interpreted as a VSCP frame:

```cpp
vscp::StreamLogSink debugLog(Serial);
vscp::StreamTransport transport(
    Serial1, vscp::MAX_MESSAGE_SIZE, &debugLog);
```

On desktop, use `vscp::IostreamLogSink` or `vscp::StdioLogSink`. The sink can be
changed later with `Transport::setLogSink()`.

## Compile-time configuration

Defaults live in `libraries/vscp/src/config.hpp` and can be overridden with
compiler definitions:

| Definition | Default | Meaning |
| --- | --- | --- |
| `VSCP_API_VERSION` | `"1.4"` | API version sent by `Client::init()` |
| `MAX_PROTOCOL_REQUEST_SIZE` | `1024` | Maximum protocol frame size |
| `PROTOCOL_INIT_TIMEOUT` | `500` | Default client response timeout in milliseconds |
| `PROTOCOL_VERBOSE` | `1` | Diagnostic verbosity |
| `VSCP_ENABLE_IOSTREAM` | `1` | Include the desktop C++ stream adapter |
| `VSCP_ENABLE_STDIO` | `1` | Include the desktop C stdio adapter |

The environment is selected automatically: Arduino builds define
`ARDUINO_H_ENV`, while desktop builds default to `STDIO_H_ENV`. Defining both is
an error.

For implementation-specific notes and upstream provenance, see
[`libraries/vscp/README.md`](libraries/vscp/README.md) and
[`libraries/vscp/UPSTREAM.md`](libraries/vscp/UPSTREAM.md).
