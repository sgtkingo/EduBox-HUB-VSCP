# API 1.6 revision: optional ordinary command seq

The API string remains 1.6. Legacy wire messages are unchanged.

Ordinary INIT/CONNECT/DISCONNECT/UPDATE/CONFIG/CONTROL/RESET requests may include
an optional decimal seq (recommended nonzero uint32). The revised server echoes
the exact attribute in every dispatched response, including error responses.
It never interprets seq as a device setting. PING retains its own existing seq
exchange; BYE remains one-way and unacknowledged.

Client::setSequenceEnabled(true), before INIT, opts into strict matching of
ordinary responses. This mode requires a revised server; responses without the
exact seq are ignored until timeout. The default is false for old servers.
The client assigns monotonically increasing seq and does not reset it on local
close/reconnection. Ordinary requests are never automatically retransmitted:
especially CONTROL must not be replayed after timeout or reconnection.

Client::closeSession() invalidates INIT/PING locally without a wire write. Physical
transport loss must call it from the same context that owns the client.
It does not abort concurrent calls: Client still has a single-threaded owner.

Malformed frames that cannot be parsed cannot reliably echo seq. A uint32 wrap
after ~4.3 billion transactions is not a durable replay-protection mechanism;
BLE generations and cleared fragment/line queues separately prevent stale data
from a previous physical link.

See tests/sequence_test.cpp for legacy interoperability, echoed errors, stale
same-UID responses, timeout and no-write local invalidation.
