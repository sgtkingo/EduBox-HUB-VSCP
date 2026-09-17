# VSCP client/server library

The library implements **Virtual Sensors Communication Protocol** API `1.6`. Library version: `2.2.2`.
It is not the event-based Very Simple Control Protocol.

## Components

- `vscp_codec.*`: shared URL-like message parser and serializer;
- `src/config.hpp`: compile-time protocol and platform defaults;
- `src/io/vscp_transport.hpp`: transport interface;
- `src/io/vscp_stream_transport.*`: non-blocking Arduino `Stream` adapter;
- `src/io/vscp_iostream_transport.*`: blocking `std::istream`/`std::ostream` adapter;
- `src/io/vscp_stdio_transport.*`: blocking C `FILE*` adapter;
- `vscp_client.*`: synchronous request client for an HMI/controller;
- `vscp_server.*`: non-blocking responder routed through command handlers.

Include all public components with `#include <vscp.hpp>`.

Configuration defaults live in `src/config.hpp` and can be overridden with
compiler definitions such as `-DMAX_PROTOCOL_REQUEST_SIZE=2048`.

## Environments

Arduino builds are detected through the `ARDUINO` macro and expose
`vscp::StreamTransport`. Desktop builds default to `STDIO_H_ENV` and expose
both `vscp::IostreamTransport` and `vscp::StdioTransport`. The desktop adapters
perform blocking line reads, which is appropriate for console applications;
an event loop should run them on a dedicated input thread.

```cpp
vscp::IostreamTransport cppTransport(std::cin, std::cout);
vscp::StdioTransport cTransport(stdin, stdout);
```

Define `VSCP_ENABLE_IOSTREAM=0` or `VSCP_ENABLE_STDIO=0` to omit either desktop
adapter. Defining both `ARDUINO_H_ENV` and `STDIO_H_ENV` is rejected.

## Transport diagnostics

`PROTOCOL_VERBOSE` controls diagnostics compiled into the common transport:

- `0`: logging disabled;
- `1`: framing and write errors;
- `2`: errors plus complete `[VSCP][RX]` and `[VSCP][TX]` frames.

A logger is explicitly injected so diagnostic output cannot silently corrupt the
protocol channel. Keep the log output and protocol output on different streams.

```cpp
// ESP32: VSCP runs on protocolUart, diagnostics use the USB console.
vscp::StreamLogSink debugLog(Serial);
vscp::StreamTransport transport(
    protocolUart, vscp::MAX_MESSAGE_SIZE, &debugLog);

// Desktop C++ streams.
vscp::IostreamLogSink desktopLog(std::clog);
vscp::IostreamTransport desktopTransport(
    std::cin, std::cout, vscp::MAX_MESSAGE_SIZE, &desktopLog);

// Desktop C streams.
vscp::StdioLogSink stdioLog(stderr);
vscp::StdioTransport stdioTransport(
    stdin, stdout, vscp::MAX_MESSAGE_SIZE, &stdioLog);
```

The sink can also be changed at runtime with `Transport::setLogSink()`.

Before dispatch, the common transport removes bytes outside printable ASCII
(`32..126`) and trims surrounding whitespace on both RX and TX. The Arduino
stream adapter also emits a separator newline and flushes each complete frame,
matching the framing behavior of the original UART messenger.

## Bidirectional PING

PING checks peer communication without INIT, device handlers, or pin changes.
Client is always `side=client`; each Server endpoint is `side=server`.

```text
?type=PING&side=client&seq=42
?side=server&seq=42&status=1
```

Either side may initiate, including simultaneously. Requests contain `type=PING`; acknowledgements contain `side`, `seq` and
`status` without `type`. These fields route acknowledgements separately; acknowledgements are never answered. `seq` is a
canonical decimal integer from 1 to 4294967295. Each endpoint generates its own
sequence, advancing for every attempt and wrapping to 1. Only `status=1` from
the opposite side with the pending sequence is accepted before the deadline.
Invalid PING fields, unsolicited, duplicate and late replies are ignored.
Parameter order is immaterial; existing commands retain their wire format.
Sequence matching applies within an endpoint lifetime, not across restarts.

```cpp
// Client: synchronous, uses the constructor's timeout, does not initialize.
auto result = client.ping();
// Client main loop: answer incoming server PING while otherwise idle.
client.poll();

// Server: non-blocking, transport must already be registered.
bool started = server.ping(transport, 1000);
server.poll();
auto progress = server.pingResult(transport);
// progress.state: Idle, Pending, Ok, Timeout, WriteError
// progress.sequence: local request sequence
```

A second server PING on the same transport is rejected while one is pending;
different transports have independent exchanges. Unknown transports cannot be
pinged. Client transactions service incoming PINGs while waiting for ordinary
responses. PING acknowledgements never satisfy an ordinary transaction.

Only one owner may read a transport. Call `poll()` and transactions from the
same execution context; these objects are not thread-safe. Client `poll()` is
for idle use and consumes one incoming frame; unrelated idle frames are discarded.
A transport must provide non-blocking reads for polling and bounded timeouts;
blocking desktop stream adapters require an appropriately managed input loop.
`Transport::writeLine()` now returns a success boolean; callers may still ignore it.

No periodic heartbeat or automatic recovery is enabled by the library.
A successful PING does not restore INIT, device connections or Panel watchdog state.

Run desktop protocol and PING regression tests:

```sh
python libraries/vscp/tests/run_tests.py
```

## BYE session notification

Servers may register a contextual handler with
`server.on(command, [](const Request& request, Transport& source) { ... })`
to implement application ownership policy per endpoint. Existing one-argument
handlers remain supported. `server.closeSession(transport)` invalidates that
endpoint and cancels its PING without sending a frame or invoking `onBye`.
Use it for physical link loss; application hardware cleanup remains explicit.
These additions do not change the API 1.6 wire format.

```text
?type=BYE&side=client
```

The server may likewise send `side=server`. BYE has no reply, status or sequence.
It works before INIT, closes only this transport's protocol session and cancels
its PING. Hardware connections/pins and the physical transport are unchanged.
Further normal requests need a new INIT; PING still works. Receiving BYE while
waiting for a response immediately returns `Peer disconnected`.

Use `client.bye()` or `server.bye(transport)` to send. The boolean result means
write success, not confirmed delivery; failed writes leave local state intact.
Use `client.sessionClosed()` to observe closure, or `server.onBye(handler)` to
observe a remote BYE with the affected `Transport&`. The callback fires once
per closed session and is rearmed by successful INIT. These methods share the
same single-owner execution requirements as poll and other transactions.
Wrong-role and status-bearing BYE messages do not close a session.
