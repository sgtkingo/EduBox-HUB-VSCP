"""Compile and run desktop protocol tests with g++ (no build artifacts in repo)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
SOURCES = [SRC / name for name in (
    "vscp_client.cpp", "vscp_server.cpp", "vscp_codec.cpp",
    "vscp_types.cpp", "io/vscp_transport.cpp",
    "io/vscp_iostream_transport.cpp", "io/vscp_stdio_transport.cpp",
)]

with tempfile.TemporaryDirectory(prefix="vscp-tests-") as directory:
    for test in sorted((ROOT / "tests").glob("*_test.cpp")):
        executable = Path(directory) / (test.stem + ".exe")
        subprocess.run([
            "g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-DSTDIO_H_ENV", "-I", str(SRC), str(test),
            *map(str, SOURCES), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=15)
        print(f"PASS {test.name}", flush=True)
