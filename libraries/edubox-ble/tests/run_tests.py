"""Portable BLE framing/VSCP tests; no radio needed, outputs outside repository."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
VSCP = ROOT.parent / "vscp" / "src"
with tempfile.TemporaryDirectory(prefix="edubox-ble-tests-") as directory:
    for test in sorted((ROOT / "tests").glob("*_test.cpp")):
        executable = Path(directory) / (test.stem + ".exe")
        subprocess.run(["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-DSTDIO_H_ENV", "-I", str(ROOT / "src"), "-I", str(VSCP),
                        str(test), *[str(VSCP / name) for name in (
                            "io/vscp_transport.cpp", "vscp_client.cpp", "vscp_server.cpp",
                            "vscp_codec.cpp", "vscp_types.cpp")], "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=15)
        print(f"PASS {test.name}", flush=True)
