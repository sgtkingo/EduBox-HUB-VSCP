#!/usr/bin/env python3
import os
import time
import threading
import traceback
from engine.emulator import VSCPEmulator, available_serial_ports, load_catalog_defaults
from engine.sensors import load_sensors

UPDATE_PERIOD_S = float(os.getenv("VSCP_UPDATE_PERIOD", "0.5"))

class RealSensorBridge:
    """Periodically pulls real data from sensor adapters and updates the emulator."""
    def __init__(self, emu: VSCPEmulator, real_sensors):
        self.emu = emu
        self.sensors = real_sensors
        self.running = False
        # Pre-register keys so UPDATE works before first sample arrives
        for s in self.sensors:
            self.emu.sensor_data.setdefault(s.uid, {"type": s.kind})
            
        

    def loop(self):
        while self.running:
            for s in self.sensors:
                try:
                    data = s.read()
                    if isinstance(data, dict) and data:
                        payload = {**data, "type": s.kind}
                        self.emu.sensor_data[s.uid] = payload
                    else:
                        #print(f"[Sensor {s.uid}] no data found, using simulated values...")
                        pass
                except Exception as e:
                    print(f"EXCEPTION: Real sensor read failed reason={e} source=RealSensorBridge.loop sensor={s.uid}")
                    traceback.print_exc()
            time.sleep(UPDATE_PERIOD_S)

    def start(self):
        self.running = True
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def stop(self):
        self.running = False
        if hasattr(self, "t"):
            self.t.join(timeout=1.0)

if __name__ == "__main__":
    # Get available COM ports
    available_ports = available_serial_ports()
    
    if available_ports:
        default_port = available_ports[0]
        print(f"Available COM ports: {', '.join(available_ports)}")
    else:
        default_port = 'COM8'
        print("No COM ports detected, using default COM8")
    
    port = input(f"Enter serial port (default: {default_port}): ").strip()
    if not port:
        port = default_port
        
    # Load the same device catalog that the firmware uses. Real adapters overwrite
    # matching runtime values as samples arrive.
    virtual_sensors, metadata = load_catalog_defaults()
    if not virtual_sensors:
        print("No catalog devices found. Exiting.")
        raise SystemExit(1)
    print(f"Using catalog {metadata.get('path', 'fallback defaults')}")
    print(f"Catalog app={metadata.get('application')} db={metadata.get('version')}")
    emu = VSCPEmulator(virtual_sensors, port=port, baudrate=115200)

    try:
        if not emu.connect_serial():
            raise SystemExit(1)
        emu.running = True

        threading.Thread(target=emu.listen_loop, daemon=True).start()

        # The firmware initializes the protocol with:
        # ?type=INIT&app=<catalog application>&db=<catalog version>&api=1.6

        # Start real sensor bridge
        real_sensors = load_sensors()
        bridge = RealSensorBridge(emu, real_sensors)
        bridge.start()

        print("Real sensors active:", [f"{s.uid}({s.kind})" for s in real_sensors])
        print("Send INIT first, then UPDATE, e.g.: ?type=UPDATE&id=mic_001 | ?type=UPDATE&id=S01")

        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Shutting down real sensor emulator...")
    except Exception as exc:
        print(f"EXCEPTION: real_runner failed reason={exc} source=real_runner")
        traceback.print_exc()
    finally:
        if "bridge" in locals():
            bridge.stop()
        emu.running = False
        emu.disconnect_serial()
