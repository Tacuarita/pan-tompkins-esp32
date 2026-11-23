#!/usr/bin/env python3
"""Send a canonical INCART excerpt to the ESP32-S3 and verify its replay report."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
from select import select
import subprocess
import sys
import termios
import time
import tty
import zlib

FRAME_BYTES = 12
TRIPLET_BYTES = FRAME_BYTES // 2
RATE_HZ = 257
RING_CAPACITY = 64
MODES = ("normal", "underrun", "overflow")
LEADS = ("I", "II", "III", "aVR", "aVL", "aVF")
PLACEMENTS = {"A": (0, 1, 1), "B": (0, 1, 0)}
DEFAULT_MANIFEST = Path("data/replay/I01_0_7710_scale1024.json")


def load_payload(manifest_path):
    manifest = json.loads(manifest_path.read_text())
    expected = {
        "format_version": 1,
        "bytes_per_frame": FRAME_BYTES,
        "sample_rate_hz": RATE_HZ,
        "code_type": "int16",
        "byte_order": "little",
        "lead_order": list(LEADS),
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise ValueError(f"unsupported replay format for {key}")
    payload_path = manifest_path.with_name(manifest["payload_file"])
    payload = payload_path.read_bytes()
    if (
        not payload
        or len(payload) % FRAME_BYTES
        or len(payload) != manifest["frame_count"] * FRAME_BYTES
        or len(payload) != manifest["payload_bytes"]
    ):
        raise ValueError("replay length does not match manifest")
    if hashlib.sha256(payload).hexdigest() != manifest["payload_sha256"]:
        raise ValueError("replay SHA-256 does not match manifest")
    return payload, payload_path


def offline_candidates(detector, payload_path):
    candidates = []
    for lead in LEADS:
        output = subprocess.check_output(
            [detector.resolve(), payload_path, lead], text=True, encoding="ascii"
        ).splitlines()
        if not output or output[0] != "lead,beat_location,production_frame":
            raise ValueError(f"invalid offline candidate header for {lead}")
        sequence = []
        for row in output[1:]:
            name, location, frame = row.split(",")
            if name != lead:
                raise ValueError(f"unexpected offline lead {name!r}, expected {lead}")
            sequence.append((int(location), int(frame)))
        candidates.append(sequence)
    return candidates


def verify_parity(config, placement, actual, expected):
    if placement != PLACEMENTS[config]:
        raise ValueError(
            f"configuration {config} expected placement {PLACEMENTS[config]}, observed {placement}"
        )
    for lead, observed, reference in zip(LEADS, actual, expected):
        if observed != reference:
            for index, (board, offline) in enumerate(zip(observed, reference), 1):
                if board != offline:
                    raise ValueError(
                        f"{lead} candidate {index} differs: board {board}, offline {offline}"
                    )
            raise ValueError(
                f"{lead} candidate count differs: board {len(observed)}, offline {len(reference)}"
            )
    counts = ", ".join(f"{lead} {len(rows)}" for lead, rows in zip(LEADS, actual))
    print(f"configuration {config} placement {placement}, exact candidate parity ({counts})")


def configure_port(port):
    fcntl.ioctl(port, termios.TIOCEXCL)
    tty.setraw(port)
    settings = termios.tcgetattr(port)
    settings[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    settings[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    settings[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    settings[4] = settings[5] = termios.B115200
    termios.tcsetattr(port, termios.TCSANOW, settings)
    termios.tcflush(port, termios.TCIOFLUSH)


def write_all(port, data):
    remaining = memoryview(data)
    deadline = time.monotonic() + 5
    while remaining:
        _, writable, _ = select([], [port], [], max(0, deadline - time.monotonic()))
        if not writable:
            raise TimeoutError("UART write timed out")
        remaining = remaining[os.write(port, remaining):]


def wait_for_board(port):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        write_all(port, b"\nPING\n")
        for line in board_lines(port, timeout=0.25):
            if line == b"READY 1":
                return
    raise TimeoutError("ESP32-S3 did not acknowledge the UART handshake")


def board_lines(port, timeout=8):
    pending = bytearray()
    while True:
        readable, _, _ = select([port], [], [], timeout)
        if not readable:
            return
        chunk = os.read(port, 4096)
        if not chunk:
            raise OSError("UART disconnected")
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, pending = pending.partition(b"\n")
            yield line.strip()
        if len(pending) > 4096:
            raise ValueError("oversized UART response")


def triplet_crc(payload, worker, frames):
    crc = 0
    offset = worker * TRIPLET_BYTES
    for index in range(frames):
        start = index * FRAME_BYTES + offset
        crc = zlib.crc32(payload[start:start + TRIPLET_BYTES], crc)
    return crc


def verify_result(report, mode, payload, sent):
    count = len(payload) // FRAME_BYTES
    expected = {
        "normal": ("OK", count, count, (count, count)),
        "underrun": ("INPUT_UNDERRUN", 1, 1, (1, 1)),
        "overflow": ("RING_OVERFLOW", RING_CAPACITY, RING_CAPACITY + 1, (RING_CAPACITY, 0)),
    }
    expected_status, expected_published, expected_sent, expected_workers = expected[mode]
    fields = report.split()
    if len(fields) != 12 or fields[1].decode("ascii") != expected_status:
        raise ValueError(f"{mode} expected {expected_status}, received {report!r}")
    published, scheduled, actual, max_late = map(int, fields[2:6])
    if (published, sent) != (expected_published, expected_sent):
        raise ValueError(f"{mode} published or sent frame count differs from expected")
    last_release = published - 1 if mode == "normal" else published
    if (
        scheduled != last_release * 1_000_000 // RATE_HZ
        or max_late < 0
        or actual < 0
        or abs(actual - scheduled) > max_late
    ):
        raise ValueError(f"{mode} inconsistent board timestamps")
    if mode == "normal" and max_late >= 1_000_000 // RATE_HZ:
        raise ValueError("a normal replay release was at least one frame period late")

    for worker, expected_frames in enumerate(expected_workers):
        offset = 6 + 3 * worker
        frames = int(fields[offset])
        index_errors = int(fields[offset + 1])
        observed_crc = int(fields[offset + 2], 16)
        expected_crc = triplet_crc(payload, worker, expected_frames)
        if (frames, index_errors, observed_crc) != (expected_frames, 0, expected_crc):
            raise ValueError(f"{mode} worker {worker + 1} frame count, order or CRC mismatch")


def run_trial(port, replies, mode, payload, config=None, reference=None):
    count = len(payload) // FRAME_BYTES
    send_limit = {"normal": count, "underrun": 1, "overflow": RING_CAPACITY + 1}[mode]
    if count < send_limit or (mode == "underrun" and count < 2):
        raise ValueError(f"excerpt has too few frames for {mode}")

    trial = "parity" if config else mode
    write_all(port, f"START {count} {trial}\n".encode("ascii"))
    sent = 0
    placement = None
    candidates = [[] for _ in LEADS]
    for reply in replies:
        if reply.startswith(b"GRANT "):
            fields = reply.split()
            if len(fields) != 2:
                raise ValueError(f"invalid board credit: {reply!r}")
            granted = int(fields[1])
            end = sent + granted
            if not 0 < granted <= 128 or end > send_limit:
                raise ValueError(f"unexpected board credit: {reply!r}")
            write_all(port, payload[sent * FRAME_BYTES:end * FRAME_BYTES])
            sent = end
        elif reply.startswith(b"RESULT "):
            print(f"{mode} {reply.decode('ascii')}", flush=True)
            verify_result(reply, mode, payload, sent)
            if config:
                verify_parity(config, placement, candidates, reference)
            return
        elif config and reply.startswith(b"PLACEMENT "):
            if sent != count or placement is not None:
                raise ValueError(f"unexpected placement report: {reply!r}")
            placement = tuple(map(int, reply.split()[1:]))
        elif config and reply.startswith(b"CANDIDATE "):
            lead, location, frame = map(int, reply.split()[1:])
            if placement is None or not 0 <= lead < len(LEADS) or not 0 <= location <= frame < count:
                raise ValueError(f"invalid board candidate: {reply!r}")
            candidates[lead].append((location, frame))
        elif reply != b"READY 1":
            raise ValueError(f"unexpected UART reply: {reply!r}")
    raise TimeoutError("ESP32-S3 stopped responding on UART")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB UART TTY, such as /dev/ttyUSB0")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--mode", choices=(*MODES, "all"), default="normal")
    parser.add_argument("--configuration", choices=PLACEMENTS, help="verify the board's scalar placement")
    parser.add_argument("--scalar-replay", type=Path, help="host scalar replay executable for exact parity")
    args = parser.parse_args()
    if bool(args.configuration) != bool(args.scalar_replay):
        parser.error("--configuration and --scalar-replay must be supplied together")
    if args.configuration and args.mode not in ("normal", "all"):
        parser.error("candidate parity requires a complete normal replay")

    try:
        payload, payload_path = load_payload(args.manifest)
        reference = offline_candidates(args.scalar_replay, payload_path) if args.scalar_replay else None
        port = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            configure_port(port)
            wait_for_board(port)
            replies = board_lines(port)
            for mode in MODES if args.mode == "all" else (args.mode,):
                config = args.configuration if mode == "normal" else None
                run_trial(port, replies, mode, payload, config, reference)
        finally:
            os.close(port)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"UART replay failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
