# VSCP Protocol

Current API version: `1.5`. VSCP library version: `2.1.0`.

**VSCP** (*Virtual Sensors Communication Protocol*) is a simple text-based protocol for exchanging messages between a controller application and a target device. The protocol is designed for scenarios where sensor values need to be read, actuators need to be controlled, device parameters need to be configured, and connections to physical or logical pins need to be confirmed.

VSCP uses a **request-response** model: one side sends one command, and the other side responds with one message. All commands use the same text format.

## 1. Basic Properties

- human-readable text protocol,
- synchronous `request -> response` model,
- messages in a URL query string-like format,
- all values are transmitted as text strings,
- every response contains a `status` field,
- commands distinguish initialization, connection, reading, configuration, control, and reset.

The protocol can be carried over any transport that preserves line-separated text messages, typically UART, USB serial, TCP socket, or another stream-based channel.

## 2. Transport Layer

VSCP does not define a specific physical or link layer. It only assumes that one message is transmitted as one standalone text line.

Recommended general requirements:

| Property | Recommendation |
| --- | --- |
| Encoding | ASCII or UTF-8 without control characters inside values |
| Message termination | `\n` |
| Communication model | synchronous request-response |
| Parallel requests | not recommended without an additional `sequence id` |
| Timeout | implementation-defined |

Example of a single message:

```text
?type=UPDATE&id=temp_sensor
```

## 3. Wire Format

A VSCP message has the format of a URL-like query string:

```text
?key=value&key2=value2
```

Rules:

- the message starts with the `?` character,
- parameters are separated by the `&` character,
- key and value are separated by the first `=` character,
- keys should not be empty,
- values are transmitted as strings,
- key names are recommended to be case-sensitive,
- values containing `&`, `=`, spaces, or special characters should be URL-encoded if supported by the implementation,
- without URL encoding, only simple alphanumeric values are safe to use.

Example:

```text
?type=CONTROL&id=led_01&brightness=80
```

## 4. Response Status

Every response contains the `status` parameter.

| Value | Meaning |
| --- | --- |
| `status=1` | the command was successfully accepted and executed |
| `status=0` | the command failed |

When an error occurs, it is recommended to include the `error` parameter.

Successful response:

```text
?id=temp_sensor&status=1&temperature=24.52
```

Error response:

```text
?id=temp_sensor&status=0&error=Device not found
```

For commands working with a specific device, the response should contain the same `id` as the request. `INIT` may be an exception because it initializes the protocol itself, not a specific device.

## 5. General Device Model

VSCP works with the general concept of a device identified by `id` or `uid`. The protocol itself does not enforce data types, ranges, or validation rules. These are the responsibility of the application layer, device catalog, or a concrete implementation.

A device may contain three typical groups of data:

| Group | Meaning | Typical command |
| --- | --- | --- |
| read values | values read from the device, such as temperature or humidity | `UPDATE` |
| write/control values | values written to the device, such as brightness, speed, or setpoint | `CONTROL` |
| config values | configuration parameters, such as mode, sensitivity, or measurement period | `CONFIG` |

Typical device roles:

| Role | Meaning |
| --- | --- |
| `sensor` | provides readable values via `UPDATE` |
| `actuator` | accepts control values via `CONTROL` |
| `hybrid` | combines reading, control, and configuration |

## 6. Command Overview

| Command | Request | Response | Purpose |
| --- | --- | --- | --- |
| `INIT` | `?type=INIT&app=<name>&db=<version>&api=<version>` | `?status=1` | protocol initialization and compatibility verification |
| `CONNECT` | `?type=CONNECT&id=<uid>&pins=<csv>` | `?id=<uid>&status=1` | confirmation of device connection to pins or channels |
| `DISCONNECT` | `?type=DISCONNECT&id=<uid>` | `?id=<uid>&status=1` | device disconnection |
| `UPDATE` | `?type=UPDATE&id=<uid>` | `?id=<uid>&status=1&key=value...` | reading current values from a device |
| `CONFIG` | `?type=CONFIG&id=<uid>&key=value...` | `?id=<uid>&status=1` | writing configuration parameters |
| `CONTROL` | `?type=CONTROL&id=<uid>&key=value...` | `?id=<uid>&status=1` | writing runtime control values |
| `RESET` | `?type=RESET&id=<uid>` | `?id=<uid>&status=1` | resetting a device or its runtime state |
| `PING` | `?type=PING&side=<client/server>&seq=<number>` | `?type=PING&side=<server/client>&seq=<number>&status=1` | bidirectional peer liveness check |

## 7. INIT

`INIT` establishes or verifies the protocol connection. It may be used to check the API version, device catalog version, or application profile.

Request:

```text
?type=INIT&app=board&db=1.0&api=1.3
```

Parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `type=INIT` | yes | command type |
| `app` | optional | application, profile, or catalog name |
| `db` | optional | device catalog or data model version |
| `api` | recommended | VSCP API version |

Successful response:

```text
?status=1
```

Error response:

```text
?status=0&error=API mismatch
```

## 8. CONNECT

`CONNECT` confirms that a given device should be associated with specific physical pins, logical channels, or other input/output endpoints.

Request:

```text
?type=CONNECT&id=heater_01&pins=3,5,6
```

Parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `type=CONNECT` | yes | command type |
| `id` | yes | device identifier |
| `pins` | profile-dependent | comma-separated list of pins or channels |

Successful response:

```text
?id=heater_01&status=1
```

Error response:

```text
?id=heater_01&status=0&error=Pin conflict
```

## 9. DISCONNECT

`DISCONNECT` disconnects a device from the current pin mapping, channels, or runtime connection.

Request:

```text
?type=DISCONNECT&id=heater_01
```

Successful response:

```text
?id=heater_01&status=1
```

Error response:

```text
?id=heater_01&status=0&error=Device not connected
```

## 10. UPDATE

`UPDATE` reads current runtime values from a device. It is typically used for sensors or readable values of hybrid devices.

Request:

```text
?type=UPDATE&id=temp_sensor
```

Successful response:

```text
?id=temp_sensor&status=1&temperature=24.52
```

The response may contain multiple values:

```text
?id=env_sensor&status=1&temperature=24.52&humidity=41&pressure=1012
```

Recommendations:

- `UPDATE` should return only values intended for reading,
- control or writable values should be set via `CONTROL`,
- unknown or unavailable values should be omitted or returned with `status=0` and an error description.

## 11. CONFIG

`CONFIG` writes device configuration values. These are parameters that define the behavior mode of a device but are not direct runtime outputs.

Request:

```text
?type=CONFIG&id=temp_sensor&period=1000&unit=C
```

Successful response:

```text
?id=temp_sensor&status=1
```

Error response:

```text
?id=temp_sensor&status=0&error=Invalid config value
```

Typical use cases:

- measurement period,
- measurement range,
- units,
- device mode,
- filtering or measurement sensitivity.

## 12. CONTROL

`CONTROL` writes runtime control values. It is used for actuators or writable values of hybrid devices.

Request:

```text
?type=CONTROL&id=led_01&brightness=80
```

Successful response:

```text
?id=led_01&status=1
```

Example of writing a regulator target value:

```text
?type=CONTROL&id=heater_01&set_point=32
?id=heater_01&status=1
```

Error example for writing a read-only value:

```text
?type=CONTROL&id=temp_sensor&temperature=50
?id=temp_sensor&status=0&error=Value is not writable
```

Recommendations:

- `CONTROL` should accept only writable values,
- validation rules are defined by the application profile or device model,
- the response usually does not need to return the new value; acknowledgement through `status=1` is sufficient.

## 13. RESET

`RESET` restores the state of a device, its runtime values, or its connection according to the rules of the specific implementation.

Request:

```text
?type=RESET&id=heater_01
```

Successful response:

```text
?id=heater_01&status=1
```

Error response:

```text
?id=heater_01&status=0&error=Reset failed
```

Optionally, a profile may define a special identifier, for example:

```text
?type=RESET&id=all
```

Such an extension should be explicitly described in the application profile.

## 14. Typical Communication Scenarios

### 14.1 Sensor Example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=temp_sensor&pins=1
Device -> Controller: ?id=temp_sensor&status=1

Controller -> Device: ?type=UPDATE&id=temp_sensor
Device -> Controller: ?id=temp_sensor&status=1&temperature=24.52

Controller -> Device: ?type=DISCONNECT&id=temp_sensor
Device -> Controller: ?id=temp_sensor&status=1
```

### 14.2 Actuator Example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=led_01&pins=3
Device -> Controller: ?id=led_01&status=1

Controller -> Device: ?type=CONFIG&id=led_01&enabled=1
Device -> Controller: ?id=led_01&status=1

Controller -> Device: ?type=CONTROL&id=led_01&brightness=80
Device -> Controller: ?id=led_01&status=1
```

### 14.3 Hybrid Regulator Example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=heater_01&pins=3,5,6
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=CONFIG&id=heater_01&speed=4
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=CONTROL&id=heater_01&set_point=32
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=24

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=28

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=32
```

## 15. Error Handling

Common error states:

- unknown `type`,
- missing `id`,
- unknown device,
- invalid pin or pin conflict,
- invalid value,
- writing to a read-only value,
- reading an unavailable value,
- incompatible API version,
- timeout or incomplete response.

Recommended error format:

```text
?id=<uid>&status=0&error=<human_readable_message>
```

Example:

```text
?id=led_01&status=0&error=Brightness out of range
```

## 16. Recommended Validation Rules

An implementation should especially verify:

- that the request starts with `?`,
- that it contains `type`,
- that commands working with a device contain `id`,
- that the response contains `status`,
- that the response `id` matches the request if the command refers to a specific device,
- that `CONTROL` does not write read-only values,
- that `CONFIG` works only with configuration parameters,
- that `UPDATE` does not return internal or writable values unless explicitly allowed by the profile.

## 17. Limitations of the Basic VSCP Profile

The basic VSCP profile is simple and intentionally minimal. Therefore, it has several limitations:

- without URL encoding, values containing `&`, `=`, or spaces are not safe,
- the protocol itself does not define a checksum,
- the protocol itself does not define encryption or authentication,
- the protocol itself does not define a `sequence id`,
- parallel requests over one stream are not safe without an extension,
- data types and value ranges are outside the basic wire format,
- the specific meaning of pins and the device model must be defined by the application profile.

## 18. Minimal Compatible Implementation

A minimal VSCP implementation should support:

1. parsing a query-string message starting with `?`,
2. reading the `type` parameter,
3. generating a response with `status=1` or `status=0`,
4. support for `INIT`,
5. at least one device command, for example `UPDATE` for a sensor or `CONTROL` for an actuator,
6. error responses with the `error` parameter.

Minimal example:

```text
?type=INIT&api=1.3
?status=1

?type=UPDATE&id=temp_sensor
?id=temp_sensor&status=1&temperature=24.52
```

## 19. Bidirectional PING

PING checks peer communication before INIT, without a device ID or changes to
protocol initialization or device connections. The client sends `side=client`;
the server sends `side=server`. Either side may initiate, including simultaneously.

```text
Client -> Server: ?type=PING&side=client&seq=42
Server -> Client: ?type=PING&side=server&seq=42&status=1

Server -> Client: ?type=PING&side=server&seq=17
Client -> Server: ?type=PING&side=client&seq=17&status=1
```

Requests have no `status`. Valid requests receive `type=PING`, the responder's
`side`, the same `seq`, and `status=1`. Frames containing `status` are never
answered. Parameter order is immaterial. `seq` is a canonical decimal integer
from 1 to 4294967295, with no leading zeroes. Each endpoint advances its own
counter per attempt and wraps to 1. Only a success from the opposite role with
the pending sequence received before timeout acknowledges a local ping.
Invalid PING fields, unsolicited, duplicate and late replies are ignored.
Matching is scoped to an endpoint lifetime, not across restarts.

The server allows one pending local ping per transport; transports are independent.
Incoming requests are serviced even during ordinary client transactions.
PING acknowledgements cannot satisfy other transactions; existing commands
retain their original wire format.

The API exposes synchronous `Client::ping()`, idle `Client::poll()`, non-blocking
`Server::ping(transport, timeoutMs)` and `Server::pingResult(transport)`.
The server result contains the sequence and `Idle`, `Pending`, `Ok`, `Timeout`
or `WriteError`. A transport must have one read owner, used from a single
execution context. Polling requires non-blocking transport reads.

Heartbeat intervals and failure policy belong to the application. Success does
not imply working sensors, valid INIT or connected pins. No periodic heartbeat
or automatic connection recovery is enabled by the library.
