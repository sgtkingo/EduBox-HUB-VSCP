"""VSCP 1.6 / library 2.2.2 session framing for reactive server emulators."""

API_VERSION = "1.6"
LIBRARY_VERSION = "2.2.2"


def ping_response(params):
    sequence = params.get("seq", "")
    if (params.get("side") != "client" or "status" in params
            or not sequence or len(sequence) > 10 or sequence[0] == "0"
            or any(character not in "0123456789" for character in sequence)
            or int(sequence) > 0xFFFFFFFF):
        return None
    return {"side": "server", "seq": sequence, "status": "1"}


def handle_session_command(emulator, params):
    """None routes ordinary requests; empty string consumes notifications/replies."""
    command = params.get("type", "").upper()
    if command == "PING":
        response = ping_response(params)
        return emulator.build_message(response) if response is not None else ""
    if command == "BYE":
        if params.get("side") == "client" and "status" not in params:
            emulator.initialized = False
        return ""
    if "type" not in params and all(key in params for key in ("side", "seq", "status")):
        return ""  # A reactive emulator has no pending local PING to acknowledge.
    if command == "INIT":
        emulator.initialized = False
    elif not emulator.initialized:
        return emulator.build_message({"status": "0", "error": "Protocol not initialized"})
    return None


def send_bye(emulator):
    """Notify the client without closing the serial transport or changing pins."""
    if not emulator.ser or not emulator.ser.is_open:
        return False
    emulator.ser.write(b"?type=BYE&side=server\n")
    emulator.ser.flush()
    emulator.initialized = False
    return True
