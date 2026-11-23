#!/usr/bin/env python3
"""Generate a canonical six-lead replay excerpt from PhysioNet INCART."""

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from decimal import Decimal
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile
from urllib.request import urlopen


RELEASE = "1.0.0"
SOURCE = f"https://physionet.org/files/incartdb/{RELEASE}/"
SUMS = "SHA256SUMS.txt"
RATE = 257
LEADS = ("I", "II", "III", "aVR", "aVL", "aVF")
FRAME = struct.Struct("<6h")
MIN_CODE, MAX_CODE = -32768, 32767
ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def atomic_file(path):
    """Only replace the destination once its contents are complete."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            yield stream
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def sha256_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(name, path, validate):
    with atomic_file(path) as stream:
        with urlopen(SOURCE + name, timeout=60) as response:
            shutil.copyfileobj(response, stream)
        stream.flush()
        validate(Path(stream.name))


def read_checksums(path, names):
    checksums = {}
    for line in path.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(\S+)", line)
        if match and match[2] in names:
            if match[2] in checksums:
                raise ValueError(f"{path}: duplicate checksum for {match[2]}")
            checksums[match[2]] = match[1].lower()
    if checksums.keys() != names:
        raise ValueError(f"{path}: missing checksums for {', '.join(sorted(names - checksums.keys()))}")
    return checksums


def cache_record(record, cache, offline):
    names = {f"{record}.{suffix}" for suffix in ("hea", "dat", "atr")}
    sums_path = cache / SUMS
    try:
        checksums = read_checksums(sums_path, names)
    except (FileNotFoundError, ValueError):
        if offline:
            raise ValueError(f"offline: {sums_path} is missing or invalid") from None
        download(SUMS, sums_path, lambda path: read_checksums(path, names))
        checksums = read_checksums(sums_path, names)

    for name, expected in sorted(checksums.items()):
        path = cache / name
        if path.is_file() and sha256_file(path) == expected:
            continue
        if offline:
            raise ValueError(f"offline: {path} is missing or failed SHA-256 verification")

        def verify(candidate):
            if sha256_file(candidate) != expected:
                raise ValueError(f"downloaded {name} failed SHA-256 verification")

        download(name, path, verify)
    return checksums


@dataclass(frozen=True)
class Lead:
    column: int
    gain: Fraction
    baseline: int


def read_header(path, record):
    lines = [
        line.split() for line in path.read_text(encoding="ascii").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not lines or len(lines[0]) != 4 or lines[0][0] != record:
        raise ValueError(f"{path}: expected a single-segment INCART record")
    _, channels, rate, samples = lines[0]
    channels, samples = int(channels), int(samples)
    if Decimal(rate) != RATE or channels != 12 or samples <= 0 or len(lines) != channels + 1:
        raise ValueError(f"{path}: expected 12 leads at {RATE} Hz and a positive sample count")

    selected = {}
    labels = tuple(lead.upper() for lead in LEADS)
    for column, fields in enumerate(lines[1:]):
        if len(fields) != 9 or fields[:2] != [f"{record}.dat", "16"] or fields[7] != "0":
            raise ValueError(f"{path}: expected one interleaved WFDB format-16 signal file")
        label = fields[8].upper()
        if label not in labels:
            continue
        if label in selected:
            raise ValueError(f"{path}: duplicate lead {label}")
        calibration = re.fullmatch(r"([^(/]+)(?:\(([+-]?\d+)\))?(?:/(mV))?", fields[2])
        if calibration is None:
            raise ValueError(f"{path}: expected millivolt calibration for lead {label}")
        gain = Fraction(Decimal(calibration[1]))
        if gain <= 0:
            raise ValueError(f"{path}: expected positive gain for lead {label}")
        baseline = int(calibration[2] if calibration[2] is not None else fields[4])
        selected[label] = Lead(column, gain, baseline)
    missing = [lead for lead in LEADS if lead.upper() not in selected]
    if missing:
        raise ValueError(f"{path}: missing leads {', '.join(missing)}")
    return struct.Struct(f"<{channels}h"), samples, [selected[label] for label in labels]


def round_ratio(numerator, denominator):
    """Round to nearest, with exactly half a count rounded away from zero."""
    magnitude, remainder = divmod(abs(numerator), denominator)
    rounded = magnitude + (2 * remainder >= denominator)
    return -rounded if numerator < 0 else rounded


def encode(signal_path, source_frame, start, stop, samples, leads, scale):
    if not 0 <= start < stop <= samples:
        raise ValueError(f"expected 0 <= start < stop <= {samples}")
    with signal_path.open("rb") as stream:
        if stream.seek(0, 2) != samples * source_frame.size:
            raise ValueError(f"{signal_path}: byte length disagrees with the WFDB header")
        stream.seek(start * source_frame.size)
        source_bytes = stream.read((stop - start) * source_frame.size)
    if len(source_bytes) != (stop - start) * source_frame.size:
        raise ValueError(f"{signal_path}: truncated excerpt")

    ratios = [Fraction(scale) / lead.gain for lead in leads]
    clipped = dict.fromkeys(LEADS, 0)
    payload = bytearray((stop - start) * FRAME.size)
    for index, source in enumerate(source_frame.iter_unpack(source_bytes)):
        values = []
        for label, lead, ratio in zip(LEADS, leads, ratios):
            raw = source[lead.column]
            if raw == MIN_CODE:
                raise ValueError(f"{signal_path}: missing sample at {start + index}, lead {label}")
            code = round_ratio((raw - lead.baseline) * ratio.numerator, ratio.denominator)
            if code < MIN_CODE or code > MAX_CODE:
                clipped[label] += 1
                code = max(MIN_CODE, min(MAX_CODE, code))
            values.append(code)
        FRAME.pack_into(payload, index * FRAME.size, *values)
    return payload, clipped


def generate(args):
    checksums = cache_record(args.record, args.cache_dir, args.offline)
    header = args.cache_dir / f"{args.record}.hea"
    signal = args.cache_dir / f"{args.record}.dat"
    source_frame, samples, leads = read_header(header, args.record)
    payload, clipped = encode(signal, source_frame, args.start, args.stop, samples, leads, args.scale)
    stem = f"{args.record}_{args.start}_{args.stop}_scale{args.scale}"
    binary = args.output_dir / f"{stem}.bin"
    manifest_path = args.output_dir / f"{stem}.json"
    manifest = {
        "format_version": 1,
        "dataset": "incartdb",
        "dataset_version": RELEASE,
        "source_url": SOURCE,
        "source_sha256": checksums,
        "record": args.record,
        "annotation_file": f"{args.record}.atr",
        "sample_rate_hz": RATE,
        "source_start_sample": args.start,
        "source_stop_sample_exclusive": args.stop,
        "frame_count": args.stop - args.start,
        "lead_order": LEADS,
        "physical_unit": "mV",
        "code_type": "int16",
        "byte_order": "little",
        "bytes_per_frame": FRAME.size,
        "counts_per_mV": args.scale,
        "rounding": "nearest, ties away from zero",
        "saturation_range": [MIN_CODE, MAX_CODE],
        "clipped_samples_by_lead": clipped,
        "calibration_by_lead": {
            label: {"adc_units_per_mV": str(lead.gain), "baseline_adc": lead.baseline}
            for label, lead in zip(LEADS, leads)
        },
        "payload_file": binary.name,
        "payload_bytes": len(payload),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
    }
    with atomic_file(binary) as stream:
        stream.write(payload)
    with atomic_file(manifest_path) as stream:
        stream.write((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(f"{manifest['payload_sha256']}  {binary}")
    print(f"Manifest: {manifest_path}")
    print(f"Host annotations: {args.cache_dir / manifest['annotation_file']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", default="I01", help="INCART record I01–I75 (default: I01)")
    parser.add_argument("--start", type=int, default=0, help="first source frame (default: 0)")
    parser.add_argument("--stop", type=int, default=30 * RATE, help="exclusive source frame (default: 7710)")
    parser.add_argument("--scale", type=int, default=1024, help="counts per millivolt (default: 1024)")
    parser.add_argument("--cache-dir", type=Path, default=ROOT / "data" / "incartdb" / RELEASE)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "data" / "replay")
    parser.add_argument("--offline", action="store_true", help="require verified local files and disable network access")
    args = parser.parse_args()
    if not re.fullmatch(r"I(?:0[1-9]|[1-6][0-9]|7[0-5])", args.record):
        parser.error("--record must be in I01–I75")
    if args.scale <= 0:
        parser.error("--scale must be positive")
    if not 0 <= args.start < args.stop:
        parser.error("expected 0 <= --start < --stop")
    try:
        generate(args)
    except (OSError, ValueError, ArithmeticError) as error:
        parser.exit(1, f"incart_replay: {error}\n")


if __name__ == "__main__":
    main()
