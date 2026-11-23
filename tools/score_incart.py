#!/usr/bin/env python3
"""Score offline scalar fused beats against the cached INCART annotations."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from incart_replay import RATE, RELEASE, ROOT, sha256_file
from uart_replay import load_payload, offline_candidates, offline_fusion


TOLERANCE_MS = 150
TOLERANCE_SAMPLES = TOLERANCE_MS * RATE // 1000
# Codes accepted by the default WFDB isqrs mapping in lib/ecgmap.h.
QRS_CODES = frozenset(range(1, 14)) | {25, 30, 31, 34, 35, 38, 41}


def score_beats(references, estimates, start, stop):
    """Score both event sequences in the half-open source sample window."""
    if not 0 <= start < stop:
        raise ValueError("expected 0 <= start < stop")
    references = sorted(sample for sample in references if start <= sample < stop)
    estimates = sorted(sample for sample in estimates if start <= sample < stop)
    ref_index = est_index = tp = 0
    # Match the earliest compatible pair. For sorted samples and a fixed
    # tolerance, exchanging later pairs for this pair preserves match count.
    while ref_index < len(references) and est_index < len(estimates):
        delta = estimates[est_index] - references[ref_index]
        if delta < -TOLERANCE_SAMPLES:
            est_index += 1
        elif delta > TOLERANCE_SAMPLES:
            ref_index += 1
        else:
            ref_index += 1
            est_index += 1
            tp += 1
    return {
        "true_positives": tp,
        "false_negatives": len(references) - tp,
        "false_positives": len(estimates) - tp,
        "sensitivity": tp / len(references) if references else None,
        "positive_predictive_value": tp / len(estimates) if estimates else None,
    }


def reference_beats(manifest, cache_dir):
    import wfdb

    filename = manifest["annotation_file"]
    record = manifest["record"]
    if Path(record).name != record or filename != f"{record}.atr":
        raise ValueError("expected an INCART record name and its .atr file")
    annotation_hash = manifest["source_sha256"][filename]
    if sha256_file(cache_dir / filename) != annotation_hash:
        raise ValueError(f"{filename}: SHA-256 does not match the replay manifest")
    annotation = wfdb.rdann(str(cache_dir / record), "atr", return_label_elements=["label_store"])
    samples = [
        int(sample) for sample, code in zip(annotation.sample, annotation.label_store, strict=True)
        if code in QRS_CODES
    ]
    return samples, annotation_hash, wfdb.__version__


def score_replay(args):
    manifest = json.loads(args.manifest.read_text())
    if (manifest.get("dataset"), manifest.get("dataset_version")) != ("incartdb", RELEASE):
        raise ValueError(f"expected an INCART {RELEASE} replay manifest")
    _, payload_path = load_payload(args.manifest)
    frames = manifest["frame_count"]
    replay_start = manifest["source_start_sample"]
    replay_stop = manifest["source_stop_sample_exclusive"]
    if replay_start < 0 or replay_stop - replay_start != frames:
        raise ValueError("source replay bounds do not match frame count")
    if not replay_start <= args.start < args.stop <= replay_stop:
        raise ValueError("scored window must be nonempty and inside the replay")
    if replay_stop - args.stop < args.window + args.max_delay:
        raise ValueError("replay must include at least W + H real post-roll frames")

    references, annotation_hash, wfdb_version = reference_beats(manifest, args.cache_dir)
    candidates = offline_candidates(args.scalar_replay, payload_path)
    late_count, locations = offline_fusion(candidates, frames, args.window, args.max_delay)
    sequence = "".join(f"{location}\n" for location in locations).encode("ascii")
    return {
        "record": manifest["record"],
        "sample_rate_hz": RATE,
        "source_window": [args.start, args.stop],
        "source_replay": [replay_start, replay_stop],
        "pre_roll_samples": args.start - replay_start,
        "post_roll_samples": replay_stop - args.stop,
        "tolerance_ms": TOLERANCE_MS,
        "tolerance_samples": TOLERANCE_SAMPLES,
        "payload_sha256": manifest["payload_sha256"],
        "annotation_sha256": annotation_hash,
        "wfdb_version": wfdb_version,
        "implementation": "Host scalar Pan–Tompkins (scalar_replay) and uart_replay.offline_fusion",
        "scalar_replay_sha256": sha256_file(args.scalar_replay.resolve()),
        "fusion_window_samples": args.window,
        "fusion_max_delay_samples": args.max_delay,
        "late_candidates": late_count,
        "fused_replay_beats": len(locations),
        "fused_replay_locations_sha256": hashlib.sha256(sequence).hexdigest(),
        **score_beats(references, (replay_start + location for location in locations),
                      args.start, args.stop),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--scalar-replay", type=Path, required=True, help="host detector executable")
    parser.add_argument("--cache-dir", type=Path, default=ROOT / "data" / "incartdb" / RELEASE)
    parser.add_argument("--start", type=int, required=True, help="inclusive source sample")
    parser.add_argument("--stop", type=int, required=True, help="exclusive source sample")
    parser.add_argument("--window", type=int, required=True, help="fusion W in samples")
    parser.add_argument("--max-delay", type=int, required=True, help="fusion H in samples")
    args = parser.parse_args()
    if min(args.window, args.max_delay) < 0:
        parser.error("fusion W and H must be nonnegative")
    try:
        result = score_replay(args)
    except ModuleNotFoundError as error:
        if error.name != "wfdb":
            raise
        parser.exit(1, "score_incart: install WFDB with python -m pip install wfdb==4.3.1\n")
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"score_incart: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
