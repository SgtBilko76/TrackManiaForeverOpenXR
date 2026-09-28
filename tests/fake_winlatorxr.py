#!/usr/bin/env python3
"""Stand-in for WinlatorXR's XrAPI host, for testing the XrAPI build under Wine.

Creates <dir>/system, streams XrAPI 0.5 tracking lines to UDP 7872 at 72 Hz
(head slowly yawing, sync stepping 0..252 by 12), and prints the mode
packets the game sends back on UDP 7278.

    python3 tests/fake_winlatorxr.py /path/to/xr-dir [seconds] [BUTTON@START-END ...]

BUTTON is an index into the XrAPI button string (for example 10 = A), or -1
to push the left stick right, and START-END is when to hold it, in seconds
(for example 10@20-20.3).

Run the game with TMFOXR_XRAPI_DIR set to the Windows path of the same
directory (for example Z:\\path\\to\\xr-dir).
"""
import math
import os
import socket
import sys
import time

SYNC_STEP = 12
SYNC_LIMIT = 256


def tracking_line(t: float, sync: int, presses) -> str:
    yaw = math.radians(20.0) * math.sin(t * 0.5)
    head = (0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2))
    left = (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -0.2, 1.1, -0.3)
    right = (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.2, 1.1, -0.3)
    hmd = head + (0.0, 1.6, 0.0)
    left_stick_x = 0.0
    for index, start, end in presses:
        if index == -1 and start <= t < end:
            left_stick_x = 0.8
    left = left[:4] + (left_stick_x, 0.0) + left[6:]
    floats = left + right + hmd + (0.064, 104.0, 98.0)
    buttons = ["F"] * 19
    for index, start, end in presses:
        if index >= 0 and start <= t < end:
            buttons[index] = "T"
    fields = ["client0"] + [f"{v:.3f}" for v in floats] + [str(sync), "".join(buttons)]
    fields += ["1.600"] + ["0.000", "0.000", "0.000", "1.000"] * 2
    return " ".join(fields) + " FT"


def main() -> None:
    directory = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
    presses = []
    for spec in sys.argv[3:]:
        index, window = spec.split("@")
        start, end = window.split("-")
        presses.append((int(index), float(start), float(end)))
    os.makedirs(directory, exist_ok=True)
    for name in os.listdir(directory):
        os.remove(os.path.join(directory, name))
    with open(os.path.join(directory, "system"), "w") as f:
        f.write("FAKE\nWINLATORXR\n14\n2026-09-01\n1920x1080\n")

    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 7278))
    receiver.setblocking(False)

    start = time.monotonic()
    sync = 0
    last_mode = None
    version_seen = False
    while time.monotonic() - start < duration:
        t = time.monotonic() - start
        sender.sendto(tracking_line(t, sync, presses).encode(), ("127.0.0.1", 7872))
        sync += SYNC_STEP
        if sync >= SYNC_LIMIT:
            sync = 0
        try:
            while True:
                mode = receiver.recv(256).decode()
                if mode != last_mode:
                    print(f"[{t:6.1f}s] mode from game: {mode!r}", flush=True)
                    last_mode = mode
        except BlockingIOError:
            pass
        version = os.path.join(directory, "version")
        if not version_seen and os.path.exists(version):
            with open(version) as f:
                print(f"[{t:6.1f}s] game requested XrAPI {f.read().strip()!r}", flush=True)
            version_seen = True
        time.sleep(1 / 72)


if __name__ == "__main__":
    main()
