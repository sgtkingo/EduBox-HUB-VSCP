#!/usr/bin/env python3
"""
Virtual Sensors Communication Protocol (VSCP) emulator.

This module mirrors the protocol implemented by libraries/vscp/src/protocol.cpp.
The wire format is a URL-like query string:

    ?type=UPDATE&id=S01
    ?id=S01&status=1&temp=24&humi=58

Supported API 1.3 requests:
INIT, UPDATE, CONFIG, CONTROL, RESET, CONNECT, DISCONNECT.
"""

from __future__ import annotations

import json
import importlib
import random
import threading
import time
import traceback
from pathlib import Path
from typing import Any, Dict, Optional
from urllib.parse import unquote

try:
    import serial
except ModuleNotFoundError:
    serial = None


PROTOCOL_API_VERSION = "1.5"
DEFAULT_DB_VERSION = "1.0"
DEFAULT_APP_NAME = "board"


def is_firmware_log_line(line: str) -> bool:
    upper_line = line.upper()
    return "DEBUG" in upper_line or "WARNING" in upper_line or "WARN:" in upper_line or "EXCEPTION" in upper_line


def find_vscp_request_start(line: str) -> int:
    return line.lower().find("?type=")


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _catalog_path() -> Optional[Path]:
    for candidate in (_repo_root() / "data" / "DB.json", _repo_root() / "ui" / "data" / "DB.json"):
        if candidate.exists():
            return candidate
    return None


def _stringify(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def available_serial_ports() -> list[str]:
    if serial is None:
        return []

    try:
        list_ports = importlib.import_module("serial.tools.list_ports")
    except Exception:
        return []

    return [port.device for port in list_ports.comports()]


def load_catalog_defaults(path: Optional[str | Path] = None) -> tuple[Dict[str, Dict[str, Any]], Dict[str, Any]]:
    """Load emulator device defaults from the project device catalog."""
    catalog_file = Path(path) if path else _catalog_path()
    if not catalog_file or not catalog_file.exists():
        return default_sensor_values(), {
            "application": DEFAULT_APP_NAME,
            "version": DEFAULT_DB_VERSION,
        }

    with catalog_file.open("r", encoding="utf-8") as fh:
        catalog = json.load(fh)

    devices: Dict[str, Dict[str, Any]] = {}
    for uid, device in catalog.get("devices", {}).items():
        defaults = device.get("default", {})
        payload: Dict[str, Any] = {}
        value_access: Dict[str, str] = {}
        control_defaults: Dict[str, Any] = {}
        default_values = defaults.get("values", {})

        for key, schema in device.get("values", {}).items():
            value = default_values.get(key, schema.get("value", 0))
            payload[key] = value
            access = str(schema.get("access", schema.get("direction", "read"))).lower()
            if access in {"write", "out", "output", "control"}:
                access = "write"
                control_defaults[key] = value
            else:
                access = "read"
            value_access[key] = access

        restrictions = {}
        for key, schema in device.get("values", {}).items():
            schema_restrictions = schema.get("restrictions", {})
            if schema_restrictions:
                restrictions[key] = schema_restrictions
        if restrictions:
            payload["_restrictions"] = restrictions
        if value_access:
            payload["_value_access"] = value_access
        if control_defaults:
            payload["_control_values"] = control_defaults

        payload["type"] = device.get("type", uid)

        configs = {}
        for key, schema in device.get("configs", {}).items():
            default_configs = defaults.get("configs", {})
            configs[key] = default_configs.get(key, schema.get("value", ""))

        if configs:
            payload["_configs"] = configs

        role = device.get("role", "sensor")
        payload["_role"] = role
        devices[uid] = payload

    return devices, {
        "application": catalog.get("application", DEFAULT_APP_NAME),
        "version": str(catalog.get("version", DEFAULT_DB_VERSION)),
        "path": str(catalog_file),
    }


def default_sensor_values() -> Dict[str, Dict[str, Any]]:
    """Fallback catalog matching the current bundled DB.json."""
    return {
        "mic_001": {"dBFS": 0.0, "peak": 0.0, "type": "SLM (dBFS)", "_role": "sensor"},
        "cam_001": {"lux_est": 0.0, "type": "CAM Lux meter", "_role": "sensor"},
        "cpu_temp": {"temp": 0.0, "type": "CPU Temp", "_role": "sensor"},
        "S00": {"temp": 0.0, "alarm": "0", "type": "DS18B20", "_configs": {"Res": 2}, "_role": "sensor"},
        "S01": {"temp": 0, "humi": 0, "type": "DHT11", "_configs": {"Unit": "C"}, "_role": "sensor"},
        "S15": {"intensity": 0, "type": "PhotoResistor", "_configs": {"Res": 5}, "_role": "sensor"},
        "A00": {
            "Brightness": 40,
            "type": "PWM LED Driver",
            "_configs": {"Enabled": "1"},
            "_control_values": {"Brightness": 40},
            "_restrictions": {"Brightness": {"min": 0, "max": 100, "step": 5}},
            "_value_access": {"Brightness": "write"},
            "_role": "actuator",
        },
        "H00": {
            "set_point": 25,
            "temp": 20,
            "type": "Temperature Regulator",
            "_configs": {"speed": 2},
            "_control_values": {"set_point": 25},
            "_restrictions": {
                "set_point": {"min": 5, "max": 80, "step": 1},
                "temp": {"min": -40, "max": 120},
            },
            "_value_access": {"set_point": "write", "temp": "read"},
            "_role": "hybrid",
        },
    }


class VSCPEmulator:
    """Reactive VSCP endpoint used by virtual and real sensor runners."""

    def __init__(
        self,
        sensors: Optional[Dict[str, Dict[str, Any]]] = None,
        port: str = "COM3",
        baudrate: int = 115200,
        timeout: float = 0.1,
        api_version: str = PROTOCOL_API_VERSION,
        db_version: Optional[str] = None,
        app_name: Optional[str] = None,
        strict_api: bool = True,
    ):
        catalog_sensors, metadata = load_catalog_defaults()

        self.API_VERSION = api_version
        self.DB_VERSION = db_version or metadata.get("version", DEFAULT_DB_VERSION)
        self.APP_NAME = app_name or metadata.get("application", DEFAULT_APP_NAME)
        self.APP_VERSION = "1.0.0"
        self.strict_api = strict_api

        self.initialized = False
        self.connected_sensors: Dict[str, list[int]] = {}
        self.sensor_configs: Dict[str, Dict[str, str]] = {}
        self.control_values: Dict[str, Dict[str, str]] = {}

        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self.running = False

        self.sensor_data = sensors if sensors is not None else catalog_sensors
        for uid, payload in self.sensor_data.items():
            configs = payload.get("_configs", {})
            if isinstance(configs, dict):
                self.sensor_configs[uid] = {key: _stringify(value) for key, value in configs.items()}
            control_defaults = payload.get("_control_values", {})
            if isinstance(control_defaults, dict):
                self.control_values[uid] = {key: _stringify(value) for key, value in control_defaults.items()}

    def connect_serial(self) -> bool:
        if serial is None:
            print("pyserial is not installed. Install emulator/requirements.txt before using serial mode.")
            return False

        try:
            self.ser = serial.Serial(port=self.port, baudrate=self.baudrate, timeout=self.timeout)
            if not self.ser.is_open:
                self.ser.open()
            print(f"Connected to {self.port} at {self.baudrate} baud")
            return True
        except Exception as exc:
            print(f"Failed to connect to {self.port}: {exc}")
            return False

    def disconnect_serial(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("Serial connection closed")

    def parse_message(self, message: str) -> Dict[str, str]:
        params: Dict[str, str] = {}
        clean_message = message.strip()
        if clean_message.startswith("?"):
            clean_message = clean_message[1:]

        for pair in clean_message.split("&"):
            if "=" not in pair:
                continue
            key, value = pair.split("=", 1)
            key = key.strip()
            if key:
                params[key] = unquote(value.strip())

        return params

    def build_message(self, params: Dict[str, Any]) -> str:
        if not params:
            return "?status=0&error=No parameters"
        return "?" + "&".join(f"{key}={_stringify(value)}" for key, value in params.items())

    def _is_known_device(self, uid: str) -> bool:
        return uid in self.sensor_data

    def _require_initialized(self) -> Optional[str]:
        if self.initialized:
            return None
        return self.build_message({"status": "0", "error": "Protocol not initialized"})

    def _require_device_id(self, uid: str) -> Optional[str]:
        if not uid:
            return self.build_message({"id": uid, "status": "0", "error": "UID cannot be empty"})
        if not self._is_known_device(uid):
            return self.build_message({"id": uid, "status": "0", "error": f"Device {uid} not found"})
        return None

    def handle_init(self, params: Dict[str, str]) -> str:
        app = params.get("app", "")
        db = params.get("db", "")
        api = params.get("api", "")
        print(f"INIT request: app={app or '-'} db={db or '-'} api={api or '-'}")

        if self.strict_api and api and api != self.API_VERSION:
            return self.build_message({
                "status": "0",
                "error": f"API mismatch - got {api}, expected {self.API_VERSION}",
            })

        if db and db != self.DB_VERSION:
            print(f"DB version differs: HMI={db}, emulator={self.DB_VERSION}; accepting for emulator run")

        self.initialized = True
        return self.build_message({"status": "1"})

    @staticmethod
    def _to_float(value: Any, fallback: float) -> float:
        try:
            return float(value)
        except (TypeError, ValueError):
            return fallback

    @staticmethod
    def _to_int(value: Any, fallback: int) -> int:
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return fallback

    def _advance_temperature_regulator(self, uid: str) -> bool:
        device = self.sensor_data.get(uid, {})
        value_access = device.get("_value_access", {})
        if value_access.get("set_point") != "write" or value_access.get("temp", "read") != "read":
            return False
        if "set_point" not in device or "temp" not in device:
            return False

        current_temp = self._to_float(device.get("temp"), 20.0)
        set_point = self._to_float(self.control_values.get(uid, {}).get("set_point", device.get("set_point")), current_temp)
        speed = self._to_int(self.sensor_configs.get(uid, {}).get("speed", device.get("_configs", {}).get("speed", 2)), 2)
        speed = max(1, min(5, speed))

        delta = set_point - current_temp
        if abs(delta) <= speed:
            next_temp = set_point
        else:
            next_temp = current_temp + (speed if delta > 0 else -speed)

        device["temp"] = int(round(next_temp))
        return True

    def _runtime_payload(self, uid: str) -> Dict[str, Any]:
        payload = {}
        restrictions = self.sensor_data[uid].get("_restrictions", {})
        value_access = self.sensor_data[uid].get("_value_access", {})
        regulator_updated = self._advance_temperature_regulator(uid)
        for key, value in self.sensor_data[uid].items():
            if key.startswith("_") or key == "type":
                continue
            if value_access.get(key, "read") == "write":
                continue
            if regulator_updated and key == "temp":
                payload[key] = int(round(self._to_float(value, 0.0)))
                continue
            if isinstance(value, (int, float)):
                jitter = random.randint(-2, 2) if isinstance(value, int) else random.uniform(-0.5, 0.5)
                next_value = value + jitter
                restriction = restrictions.get(key, {})
                if "min" in restriction:
                    next_value = max(float(restriction["min"]), next_value)
                if "max" in restriction:
                    next_value = min(float(restriction["max"]), next_value)
                payload[key] = round(next_value, 2) if isinstance(value, float) else int(round(next_value))
            else:
                payload[key] = value
        return payload

    def handle_update(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        error = self._require_device_id(uid)
        if error:
            return error

        response = {"id": uid, "status": "1"}
        response.update(self._runtime_payload(uid))
        print(f"UPDATE {uid}: {response}")
        return self.build_message(response)

    def handle_config(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        error = self._require_device_id(uid)
        if error:
            return error

        config_params = {key: value for key, value in params.items() if key not in {"type", "id"}}
        self.sensor_configs.setdefault(uid, {}).update(config_params)
        print(f"CONFIG {uid}: {config_params}")
        return self.build_message({"id": uid, "status": "1"})

    def handle_control(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        error = self._require_device_id(uid)
        if error:
            return error

        value_access = self.sensor_data[uid].get("_value_access", {})
        control_params = {key: value for key, value in params.items() if key not in {"type", "id"}}
        if value_access:
            invalid = [key for key in control_params if value_access.get(key) != "write"]
            if invalid:
                return self.build_message({
                    "id": uid,
                    "status": "0",
                    "error": f"Values are not writable through CONTROL: {','.join(invalid)}",
                })

        self.control_values.setdefault(uid, {}).update(control_params)
        for key, value in control_params.items():
            if key in self.sensor_data[uid]:
                self.sensor_data[uid][key] = value
        print(f"CONTROL {uid}: {control_params}")
        return self.build_message({"id": uid, "status": "1"})

    def handle_reset(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        if uid == "all":
            self.sensor_configs.clear()
            self.control_values.clear()
            self.connected_sensors.clear()
            return self.build_message({"id": uid, "status": "1"})

        error = self._require_device_id(uid)
        if error:
            return error

        self.sensor_configs.pop(uid, None)
        self.control_values.pop(uid, None)
        self.connected_sensors.pop(uid, None)
        print(f"RESET {uid}")
        return self.build_message({"id": uid, "status": "1"})

    def handle_connect(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        pins = params.get("pins", "")
        error = self._require_device_id(uid)
        if error:
            return error

        if not pins:
            return self.build_message({"id": uid, "status": "0", "error": "Missing pins"})

        try:
            pin_list = [int(pin.strip()) for pin in pins.split(",") if pin.strip()]
        except ValueError:
            return self.build_message({"id": uid, "status": "0", "error": f"Invalid pin number: {pins}"})

        if not pin_list:
            return self.build_message({"id": uid, "status": "0", "error": "Missing pins"})

        requested = set(pin_list)
        for other_uid, used_pins in self.connected_sensors.items():
            if other_uid != uid and requested.intersection(used_pins):
                return self.build_message({
                    "id": uid,
                    "status": "0",
                    "error": f"Pins {pins} already used by device {other_uid}",
                })

        self.connected_sensors[uid] = pin_list
        print(f"CONNECT {uid}: pins={pins}")
        return self.build_message({"id": uid, "status": "1"})

    def handle_disconnect(self, params: Dict[str, str]) -> str:
        error = self._require_initialized()
        if error:
            return error

        uid = params.get("id", "")
        error = self._require_device_id(uid)
        if error:
            return error

        self.connected_sensors.pop(uid, None)
        print(f"DISCONNECT {uid}")
        return self.build_message({"id": uid, "status": "1"})

    def process_request(self, message: str) -> str:
        try:
            params = self.parse_message(message)
            request_type = params.get("type", "").upper()
            handlers = {
                "INIT": self.handle_init,
                "UPDATE": self.handle_update,
                "CONFIG": self.handle_config,
                "CONTROL": self.handle_control,
                "RESET": self.handle_reset,
                "CONNECT": self.handle_connect,
                "DISCONNECT": self.handle_disconnect,
            }

            handler = handlers.get(request_type)
            if not handler:
                return self.build_message({"status": "0", "error": f"Unknown request type: {request_type}"})
            return handler(params)
        except Exception as exc:
            print(f"EXCEPTION: Emulator request handling failed reason={exc} source=VSCPEmulator.process_request")
            traceback.print_exc()
            return self.build_message({"status": "0", "error": f"Emulator exception: {exc}"})

    def _extract_messages(self, buffer: str) -> tuple[list[tuple[str, str]], str]:
        messages = []
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.strip()
            if not line:
                continue

            if is_firmware_log_line(line):
                request_index = find_vscp_request_start(line)
                if request_index == -1:
                    messages.append(("log", line))
                    continue

                log_line = line[:request_index].strip()
                if log_line:
                    messages.append(("log", log_line))
                line = line[request_index:].strip()
                if line:
                    messages.append(("request", line))
                continue

            request_index = find_vscp_request_start(line)
            if request_index != -1:
                line = line[request_index:].strip()
                if line:
                    messages.append(("request", line))
        return messages, buffer

    def listen_loop(self):
        print("Listening for VSCP requests...")
        buffer = ""

        while self.running:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="ignore")
                    buffer += data
                    messages, buffer = self._extract_messages(buffer)

                    for kind, message in messages:
                        if kind == "log":
                            print(f"Firmware log: {message}")
                            continue

                        print(f"Received: {message}")
                        response = self.process_request(message)
                        if response:
                            self.ser.write((response + "\n").encode("utf-8"))
                            print(f"Sent: {response}")

                time.sleep(0.01)
            except Exception as exc:
                print(f"EXCEPTION: Emulator listen loop failed reason={exc} source=VSCPEmulator.listen_loop")
                traceback.print_exc()
                time.sleep(0.1)

    def run(self):
        print("Starting VSCP Emulator")
        print(f"  API Version: {self.API_VERSION}")
        print(f"  DB Version: {self.DB_VERSION}")
        print(f"  App: {self.APP_NAME}")
        print(f"  Devices: {', '.join(self.sensor_data.keys())}")

        if not self.connect_serial():
            return

        self.running = True
        listen_thread = threading.Thread(target=self.listen_loop, daemon=True)
        listen_thread.start()

        try:
            print("Emulator ready. Example: ?type=INIT&app=board&db=1.0&api=1.3")
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("Shutting down emulator...")
        finally:
            self.running = False
            self.disconnect_serial()


def main():
    available_ports = available_serial_ports()
    if available_ports:
        default_port = available_ports[0]
        print(f"Available COM ports: {', '.join(available_ports)}")
    else:
        default_port = "COM8"
        print("No COM ports detected, using default COM8")

    port = input(f"Enter serial port (default: {default_port}): ").strip() or default_port
    VSCPEmulator(port=port, baudrate=115200).run()


if __name__ == "__main__":
    main()
