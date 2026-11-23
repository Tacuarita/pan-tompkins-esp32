#!/usr/bin/env python3
"""Send a canonical INCART excerpt to the ESP32-S3 and verify its replay report."""

import argparse
from contextlib import ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
from select import select
from statistics import median
import subprocess
import sys
import termios
import time
import tty
import zlib

from replay_timing import RATE_HZ, RING_CAPACITY, TimingReport

FRAME_BYTES = 12
TRIPLET_BYTES = FRAME_BYTES // 2
MODES = ("normal", "underrun", "overflow")
LEADS = ("I", "II", "III", "aVR", "aVL", "aVF")
PLACEMENTS = {"A": (0, 1, 1), "B": (0, 1, 0), "C": (0, 1, 1), "D": (0, 1, 0)}
READY = (b"READY 1", b"READY 2")
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


def offline_fusion(candidates, frames, window, max_delay):
    eligible = []
    late = 0
    for lead, rows in enumerate(candidates):
        for location, production in rows:
            if production - location > max_delay:
                late += 1
            else:
                eligible.append((location, lead))
    eligible.sort()

    locations = []
    index = 0
    while index < len(eligible):
        anchor = eligible[index][0]
        end = anchor + window
        if end + max_delay >= frames:
            break
        leads = set()
        while index < len(eligible) and eligible[index][0] <= end:
            leads.add(eligible[index][1])
            index += 1
        if len(leads) >= 2:
            locations.append(anchor)
    return late, locations


def verify_fusion(config, report, locations, reference, frames, delayed):
    if report is None:
        raise ValueError("board did not report fusion")
    window, max_delay, late, handoffs = report
    expected_late, expected_locations = offline_fusion(reference, frames, window, max_delay)
    expected_handoffs = len({frame for rows in reference[3:] for _, frame in rows}) if delayed else 0
    if (late, handoffs) != (expected_late, expected_handoffs):
        raise ValueError(f"fusion count mismatch: expected {expected_late} late and "
                         f"{expected_handoffs} delayed handoffs, observed {late} and {handoffs}")
    if delayed and handoffs == 0:
        raise ValueError("this excerpt did not exercise a delayed handoff")
    if locations != expected_locations:
        for index, (board, offline) in enumerate(zip(locations, expected_locations), 1):
            if board != offline:
                raise ValueError(f"fused beat {index} differs: board {board}, offline {offline}")
        raise ValueError(f"fused beat count differs: board {len(locations)}, "
                         f"offline {len(expected_locations)}")

    sequence = "".join(f"{location}\n" for location in locations).encode("ascii")
    digest = hashlib.sha256(sequence).hexdigest()
    print(f"configuration {config} fusion W={window} H={max_delay}, {len(locations)} beats, "
          f"{late} late candidates, {handoffs} delayed handoffs, SHA-256 {digest}")
    return window, max_delay, locations


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
            if line in READY:
                return int(line[-1:])
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


def verify_squaring_mode(port, replies, configuration):
    expected = b"scalar" if configuration in ("A", "B") else b"simd"
    write_all(port, b"SQUARE MODE\n")
    actual = next((line for line in replies if line not in READY), b"")
    if actual != b"SQUARE MODE " + expected:
        raise ValueError(f"configuration {configuration} expected {expected!r}, received {actual!r}")


def run_squaring_benchmark(port, replies):
    write_all(port, b"SQUARE BENCH\n")
    runs = []
    for reply in replies:
        if reply == b"SQUARE OK" and len(runs) == 7:
            print(json.dumps({
                "squaring_runs": runs,
                "median_scalar_cycles_per_triplet": median(
                    run["scalar_cycles"] / run["triplets"] for run in runs
                ),
                "median_simd_cycles_per_triplet": median(
                    run["simd_cycles"] / run["triplets"] for run in runs
                ),
            }))
            return
        fields = reply.split()
        if len(fields) != 6 or fields[:2] != [b"SQUARE", b"RUN"]:
            raise ValueError(f"squaring benchmark failed: {reply!r}")
        run, triplets, scalar, simd = map(int, fields[2:])
        if run != len(runs) + 1 or run > 7 or triplets != 5793 or min(scalar, simd) <= 0:
            raise ValueError(f"invalid squaring benchmark record: {reply!r}")
        runs.append({"triplets": triplets, "scalar_cycles": scalar, "simd_cycles": simd})
    raise TimeoutError("ESP32-S3 did not finish the squaring benchmark")


def triplet_crc(payload, worker, frames):
    crc = 0
    offset = worker * TRIPLET_BYTES
    for index in range(frames):
        start = index * FRAME_BYTES + offset
        crc = zlib.crc32(payload[start:start + TRIPLET_BYTES], crc)
    return crc


def verify_result(report, mode, payload, sent, measured=False):
    count = len(payload) // FRAME_BYTES
    expected = {
        "normal": ("OK", count, count, (count, count)),
        "underrun": ("INPUT_UNDERRUN", 1, 1, (1, 1)),
        "overflow": ("RING_OVERFLOW", RING_CAPACITY, RING_CAPACITY + 1, (RING_CAPACITY, 0)),
    }
    fields = report.split()
    if len(fields) != 12:
        raise ValueError(f"invalid board result: {report!r}")
    status = fields[1].decode("ascii")
    published, scheduled, actual, max_late = map(int, fields[2:6])
    expected_status, expected_published, expected_sent, expected_workers = expected[mode]
    unexpected_abort = measured and mode == "normal" and status in (
        "INPUT_UNDERRUN", "RING_OVERFLOW", "PREFILL_TIMEOUT", "CANDIDATE_OVERFLOW"
    )
    if unexpected_abort:
        max_published = count if status == "CANDIDATE_OVERFLOW" else count - 1
        if not 0 <= published <= min(max_published, sent) or not sent <= count:
            raise ValueError(f"inconsistent frame counts in aborted replay: {report!r}")
        expected_workers = (published, published)
    elif status != expected_status or (published, sent) != (expected_published, expected_sent):
        raise ValueError(f"{mode} expected {expected_status} and its frame counts, received {report!r}")

    last_release = published if status in ("INPUT_UNDERRUN", "RING_OVERFLOW") else max(0, published - 1)
    if (
        scheduled != last_release * 1_000_000 // RATE_HZ
        or max_late < 0
        or actual < 0
        or abs(actual - scheduled) > max_late
    ):
        raise ValueError(f"{mode} inconsistent board timestamps")
    if mode == "normal" and not measured and max_late >= 1_000_000 // RATE_HZ:
        raise ValueError("a normal replay release was at least one frame period late")

    for worker, expected_frames in enumerate(expected_workers):
        offset = 6 + 3 * worker
        frames = int(fields[offset])
        index_errors = int(fields[offset + 1])
        observed_crc = int(fields[offset + 2], 16)
        expected_crc = triplet_crc(payload, worker, expected_frames)
        if (frames, index_errors, observed_crc) != (expected_frames, 0, expected_crc):
            raise ValueError(f"{mode} worker {worker + 1} frame count, order or CRC mismatch")
    return not unexpected_abort


def run_trial(port, replies, mode, payload, config=None, reference=None, fusion_trial=None,
              timing=None, delayed=False, raw_reports=None):
    count = len(payload) // FRAME_BYTES
    send_limit = {"normal": count, "underrun": 1, "overflow": RING_CAPACITY + 1}[mode]
    if count < send_limit or (mode == "underrun" and count < 2):
        raise ValueError(f"excerpt has too few frames for {mode}")

    trial = fusion_trial or ("parity" if config else mode)
    if timing is None:
        command = f"START {count} {trial}"
    else:
        detail = "on" if timing.detailed else "off"
        command = f"MEASURE {count} {timing.start} {timing.end} {detail} {'delay' if delayed else mode}"
    write_all(port, f"{command}\n".encode("ascii"))
    sent = 0
    placement = None
    candidates = [[] for _ in LEADS]
    fusion_report = None
    memory = None
    fused = []
    for reply in replies:
        if raw_reports is not None and not reply.startswith(b"GRANT "):
            raw_reports.write(reply.decode("ascii") + "\n")
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
            expected_result = verify_result(reply, mode, payload, sent, measured=timing is not None)
            if config and expected_result:
                verify_parity(config, placement, candidates, reference)
            verified_fusion = None
            if expected_result and (fusion_trial or (timing is not None and reference is not None
                                                     and mode == "normal")):
                verified_fusion = verify_fusion(config, fusion_report, fused, reference, count,
                                                delayed or fusion_trial == "fusion-delay")
            elif timing is not None and expected_result and mode == "normal":
                if fusion_report is None:
                    raise ValueError("board did not report fusion")
                verified_fusion = fusion_report, fused
            if timing is not None:
                summary = timing.summarize(reply, fused, fusion_report, config)
                if memory is not None:
                    heap, replay, worker1, worker2, fusion_stack = memory
                    summary["min_internal_heap_bytes"] = heap
                    summary["stack_headroom_bytes"] = {
                        "replay": replay, "worker1": worker1, "worker2": worker2,
                        "fusion": fusion_stack,
                    }
                if delayed and timing.detailed and summary["deadline_misses"] == 0:
                    raise ValueError("injected delay did not cause a scored deadline miss")
                if mode == "overflow" and timing.detailed and timing.start == 0:
                    if timing.end >= RING_CAPACITY and summary["peak_backlog"] != RING_CAPACITY:
                        raise ValueError("overflow did not saturate the replay ring")
                    if timing.end == RING_CAPACITY and summary["end_backlog"] != RING_CAPACITY:
                        raise ValueError("overflow boundary did not retain the full ring backlog")
                print(json.dumps(summary, sort_keys=True), flush=True)
                if not expected_result:
                    raise ValueError(f"measured replay aborted with {summary['status']}")
                return summary, verified_fusion
            if fusion_trial:
                return verified_fusion
            return
        elif (config or timing is not None) and reply.startswith(b"PLACEMENT "):
            if sent != count or placement is not None:
                raise ValueError(f"unexpected placement report: {reply!r}")
            placement = tuple(map(int, reply.split()[1:]))
        elif (config or timing is not None) and reply.startswith(b"CANDIDATE "):
            lead, location, frame = map(int, reply.split()[1:])
            if (placement is None or fusion_report is not None or not 0 <= lead < len(LEADS)
                    or not 0 <= location <= frame < count):
                raise ValueError(f"invalid board candidate: {reply!r}")
            candidates[lead].append((location, frame))
        elif (fusion_trial or timing is not None) and reply.startswith(b"FUSION "):
            fields = reply.split()[1:]
            if placement is None or fusion_report is not None or len(fields) != 4:
                raise ValueError(f"unexpected fusion report: {reply!r}")
            fusion_report = tuple(map(int, fields))
            if min(fusion_report) < 0:
                raise ValueError(f"negative fusion parameter or count: {reply!r}")
        elif (fusion_trial or timing is not None) and reply.startswith(b"FUSED "):
            fields = reply.split()[1:]
            if fusion_report is None or len(fields) != 1:
                raise ValueError(f"unexpected fused beat: {reply!r}")
            fused.append(int(fields[0]))
        elif timing is not None and timing.detailed and reply.startswith(b"MEMORY "):
            fields = reply.split()[1:]
            if memory is not None or len(fields) != 5 or not all(field.isdigit() for field in fields):
                raise ValueError(f"invalid memory report: {reply!r}")
            memory = tuple(map(int, fields))
        elif timing is not None and reply and reply.split(maxsplit=1)[0] in (b"TIMING", b"CPU", b"FRAME", b"BEAT"):
            timing.accept(reply)
        elif reply not in READY:
            raise ValueError(f"unexpected UART reply: {reply!r}")
    raise TimeoutError("ESP32-S3 stopped responding on UART")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB UART TTY, such as /dev/ttyUSB0")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--mode", choices=(*MODES, "all"), default="normal")
    parser.add_argument("--configuration", choices=PLACEMENTS, help="verify board placement and squaring mode")
    parser.add_argument("--benchmark-squaring", action="store_true",
                        help="check and time both three-lead squaring kernels on the board before replay")
    parser.add_argument("--scalar-replay", type=Path, help="host scalar replay executable for exact parity")
    parser.add_argument("--verify-fusion", action="store_true",
                        help="compare board fusion with the offline candidates")
    parser.add_argument("--delay-fusion-handoffs", action="store_true",
                        help="repeat fusion verification with delayed handoffs from worker 2")
    parser.add_argument("--score-window", type=int, nargs=2, metavar=("START", "END"),
                        help="measure the half-open window in replay-local sample indices")
    parser.add_argument("--compare-timing", action="store_true",
                        help="repeat the same normal measured run with detailed timing disabled")
    parser.add_argument("--raw-report", type=Path,
                        help="save board records from all trials for independent inspection")
    args = parser.parse_args()
    if args.compare_timing and (not args.score_window or args.mode != "normal"
                                or args.delay_fusion_handoffs):
        parser.error("--compare-timing requires only a normal measured trial")
    if args.raw_report and not args.score_window:
        parser.error("--raw-report requires --score-window")
    if bool(args.configuration) != bool(args.scalar_replay):
        parser.error("--configuration and --scalar-replay must be supplied together")
    if args.configuration and args.mode not in ("normal", "all"):
        parser.error("candidate parity requires a complete normal replay")
    if args.verify_fusion and not args.configuration:
        parser.error("--verify-fusion requires candidate parity options")
    if args.delay_fusion_handoffs and not args.verify_fusion:
        parser.error("--delay-fusion-handoffs requires --verify-fusion")

    try:
        payload, payload_path = load_payload(args.manifest)
        if args.score_window:
            start, end = args.score_window
            if not 0 <= start < end <= len(payload) // FRAME_BYTES:
                raise ValueError("score window must be inside the replay")
        reference = offline_candidates(args.scalar_replay, payload_path) if args.scalar_replay else None
        with ExitStack() as stack:
            raw_reports = stack.enter_context(args.raw_report.open("x")) if args.raw_report else None
            port = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            stack.callback(os.close, port)
            configure_port(port)
            protocol = wait_for_board(port)
            replies = board_lines(port)
            if protocol == 1 and (args.benchmark_squaring or args.configuration in ("C", "D")):
                raise ValueError("this firmware predates SIMD squaring")
            if protocol >= 2 and args.configuration:
                verify_squaring_mode(port, replies, args.configuration)
            if args.benchmark_squaring:
                run_squaring_benchmark(port, replies)
            for mode in MODES if args.mode == "all" else (args.mode,):
                config = args.configuration if mode == "normal" else None
                fusion_trial = "fusion" if config and args.verify_fusion else None
                timing = TimingReport(start, end, detailed=True) if args.score_window else None
                normal = run_trial(port, replies, mode, payload, config, reference, fusion_trial,
                                   timing, raw_reports=raw_reports)
                if args.compare_timing:
                    baseline = run_trial(port, replies, mode, payload, config, reference, fusion_trial,
                                         TimingReport(start, end, detailed=False), raw_reports=raw_reports)
                    if normal[1] != baseline[1]:
                        raise ValueError("fusion changed with detailed timing disabled")
                    on = normal[0]["pipeline_cpu_us"]
                    off = baseline[0]["pipeline_cpu_us"]
                    print(json.dumps({"detailed_timing_cpu_overhead_us": on - off,
                                      "detailed_timing_cpu_overhead_fraction":
                                          (on - off) / off if off else None}))
                if config and args.delay_fusion_handoffs:
                    delayed_timing = TimingReport(start, end, detailed=True) if timing else None
                    delayed = run_trial(port, replies, mode, payload, config, reference, "fusion-delay",
                                        delayed_timing, delayed=True, raw_reports=raw_reports)
                    if timing:
                        normal, delayed = normal[1], delayed[1]
                    if normal != delayed:
                        raise ValueError("fusion parameters or output changed after delaying handoffs")
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"UART replay failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
