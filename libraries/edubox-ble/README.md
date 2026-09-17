# EduBox BLE transport (envelope 1)

This is a separate library, not BLE code inside VSCP. The canonical copy lives in
EduBox-HUB-VSCP/libraries/edubox-ble; Board and Panel vendor byte-identical copies.
NimBLE-Arduino is pinned to 2.5.1. See the superproject Bluetooth bridge guide for
commissioning, architecture and hardware acceptance checks.

Layers: bounded thread-safe Channel → VSCP Transport adapter → NimBLE Peripheral
or Central worker → application session policy → GUI interface.

Custom service ECB00001…D701, RX …0002 (authenticated encrypted write with response),
TX …0003 (confirmed indications), status …0004 (authenticated encrypted read).
Full UUID strings are in ble_channel.hpp. Status EDUBOX-BLE/1;ready=1 is a barrier:
the Panel sends no INIT until Board main loop has accepted secured subscription.

ATT payload never exceeds min(MTU-3,244). Eight-byte header:
EB,01,messageId LE16,offset LE16,total LE16. IDs are per direction, begin at 1,
wrap 65535→1; fragments must be contiguous and nonempty. Payload is printable
ASCII, max 1024 bytes, no newline. Queue depth four per direction. Duplicate,
missing/out-of-order, oversized, invalid ASCII, overflow, incomplete frame after
5 s, TX error or indication unconfirmed after 2 s fail the link closed. Invalid
and partial messages never reach VSCP. No automatic message retries.

writeLine success means enqueue, NOT delivery or actuator acknowledgement.
Channel.confirm advances only after ATT confirmation. Each physical link has
a local generation; disconnect clears all RX/TX/partial state and invalidates
old ACKs/callbacks. Main loop must consume loss and invalidate VSCP before open.

Callbacks copy data/change flags only. Board Peripheral.poll is nonblocking and
the application handles returned loss before server/device service. Panel Central
owns blocking GAP/GATT calls in a worker on core 0; only main loop owns VSCP/LVGL.
Radio reconnect uses a saved authenticated identity and 1/2/4/8 s backoff; it does
not restore INIT/devices or resend CONFIG/CONTROL.

Security build flags on BOTH C and C++:
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1, MYNEWT_VAL_BLE_SM_SC_ONLY=1,
MYNEWT_VAL_BLE_SM_LEGACY=0. Runtime demands encryption, MITM authentication,
bonding and a 16-byte encryption key. Stored identity is accepted only after
authentication; BLE addresses alone are not an authentication mechanism.
Bonded-only reconnect never enters a substitute PIN or accepts numeric-comparison
pairing. A missing bond requires explicit manual commissioning with the Board PIN;
automatic connections also verify the resolved identity against the saved peer.

python libraries/edubox-ble/tests/run_tests.py runs framing and real VSCP-over-
Channel integration at MTU 23. It does not test radio, NVS or physical outputs.
