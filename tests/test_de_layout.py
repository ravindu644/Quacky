#!/usr/bin/env python3
"""Simulated USB HID report assertions for a German QWERTZ host."""
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
characters = [
    ("y", 0, 0x1D), ("z", 0, 0x1C),
    ("Y", 2, 0x1D), ("Z", 2, 0x1C),
    ("ä", 0, 0x34), ("ö", 0, 0x33), ("ü", 0, 0x2F),
    ("Ä", 2, 0x34), ("Ö", 2, 0x33), ("Ü", 2, 0x2F),
    ("ß", 0, 0x2D), ("€", 0x40, 0x08),
    ("@", 0x40, 0x14), ("/", 2, 0x24),
    ("\\", 0x40, 0x2D), ("|", 0x40, 0x64),
    ("[", 0x40, 0x25), ("]", 0x40, 0x26),
    ("{", 0x40, 0x24), ("}", 0x40, 0x27),
    ("<", 0, 0x64), (">", 2, 0x64), ("\n", 0, 0x28),
]

with tempfile.TemporaryDirectory() as temp:
    folder = pathlib.Path(temp)
    (folder / "sample.txt").write_text("".join(c for c, _, _ in characters), encoding="utf-8")
    (folder / "test.qk").write_text('type_file "sample.txt"\n', encoding="utf-8")
    device = folder / "hid"
    device.touch()
    call = subprocess.run(
        [str(binary), "--quiet", "--layout", "DE", "-d", "0",
         "--hid", str(device), str(folder / "test.qk")],
        capture_output=True, text=True, check=False,
    )
    assert call.returncode == 0, call.stderr
    expected = bytearray()
    for _, mod, key in characters:
        expected.extend([mod, 0, key, 0, 0, 0, 0, 0])
        expected.extend([0] * 8)
    received = device.read_bytes()
    assert received == bytes(expected), (
        f"DE HID report mismatch: expected {len(expected)} bytes, got {len(received)}"
    )
    (folder / "sample.txt").write_text("🙂", encoding="utf-8")
    device.write_bytes(b"")
    call = subprocess.run(
        [str(binary), "--quiet", "--layout", "DE", "-d", "0",
         "--hid", str(device), str(folder / "test.qk")],
        capture_output=True, text=True, check=False,
    )
    assert call.returncode != 0, "Unsupported Unicode must fail"
    assert device.read_bytes() == b"", "Unsupported symbol emitted HID data"

print("PASS: German QWERTZ HID reports and unsupported Unicode")
