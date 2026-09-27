#!/usr/bin/env python3
"""Build/run the synthetic USB peer without touching any real device."""
import os
from pathlib import Path
import subprocess
import tempfile

tools = Path(__file__).resolve().parent.parent
include = Path(os.environ.get(
    "LIBUSB_INCLUDE", "/usr/include/libusb-1.0"))
if not (include / "libusb.h").is_file():
    include = Path("/home/admin0412/x9sp_wayland/sysroots/cortexa55-sdrv-linux/usr/include/libusb-1.0")
if not (include / "libusb.h").is_file():
    raise SystemExit("Set LIBUSB_INCLUDE to a directory containing libusb.h")

with tempfile.TemporaryDirectory(prefix="aoa-speed-test-") as directory:
    build = Path(directory)
    (build / "libusb-1.0").symlink_to(include.resolve(), target_is_directory=True)
    binary = build / "integration-test"
    subprocess.run([
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-isystem", str(build), str(tools / "tests/aoa_speed_mock_test.cpp"),
        "-l:libusb-1.0.so.0", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
