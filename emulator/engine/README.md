# VSCP Emulator – Real HW Sensor Extensions

## Install
```bash
pip install sounddevice opencv-python psutil numpy wmi
```

## Run
```bash
cd vscp_emulator_real_sensors
python real_runner.py
```

Then send commands like `?type=UPDATE&id=mic_001` to retrieve live values.

## VSCP API 1.6 / library 2.2.2

Both basic and pattern emulators support session commands before INIT:

```text
Request:  ?type=PING&side=client&seq=22
Response: ?side=server&seq=22&status=1
Notice:   ?type=BYE&side=client
```

PING validates the client role and a canonical uint32 sequence; invalid frames
and acknowledgements are consumed without replies. BYE has no reply and only
clears protocol initialization. Device pins and configuration are retained;
normal commands require a new INIT. Wrong-role/status-bearing BYE is ignored.
`emulator.bye()` sends `side=server` without closing the serial port; shutdown
also attempts this notification before physically closing the port.

Run PING/BYE regressions from the repository root:

```sh
python -m unittest emulator.engine.tests.test_ping emulator.engine.tests.test_bye
```
