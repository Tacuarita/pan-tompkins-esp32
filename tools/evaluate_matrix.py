#!/usr/bin/env python3
"""Verify and summarize the frozen ESP32-S3 matrix from its retained UART data."""

import argparse
import csv
from collections import defaultdict
from hashlib import sha256
import json
from math import comb
from pathlib import Path
import re
from statistics import median

from replay_timing import TASKS, TimingReport
from uart_replay import PLACEMENTS, load_payload, offline_candidates, offline_fusion, verify_result

CONTRASTS = ("BA", "CA", "DB", "DA")
SCORE_START, SCORE_END = 257, 7967
FRAME_COUNT = 8224
SECONDS = (SCORE_END - SCORE_START) / 257


def read_schedule(manifest, directory):
    rows = re.findall(
        r"^\| (I\d+) \| ([ABCD ]+) \| ([ABCD ]+) \| ([ABCD ]+) \|$",
        manifest.read_text(), re.M,
    )
    planned = [
        (record, str(repetition), config)
        for repetition in range(1, 4)
        for record, *orders in rows
        for config in orders[repetition - 1].split()
    ]
    if len(planned) != 72 or len(set(planned)) != 72:
        raise ValueError("expected 72 unique runs in the manifest")
    with (directory / "schedule.tsv").open() as stream:
        if list(map(tuple, csv.reader(stream, delimiter="\t"))) != planned:
            raise ValueError("recorded schedule differs from the frozen schedule")
    with (directory / "trials.tsv").open() as stream:
        observed = list(csv.DictReader(stream, delimiter="\t"))
    keys = [(row["record"], row["repetition"], row["configuration"]) for row in observed]
    if keys != planned[:len(keys)]:
        raise ValueError("trial ledger differs from the frozen trial sequence")
    return planned, observed


def verify_attempt(directory, row, payload, reference):
    config = row["configuration"]
    name = f'{row["record"]}-{row["repetition"]}-{config}'
    raw_path = directory / f"{name}.raw"
    log_path = directory / f"{name}.log"
    messages = log_path.read_text().splitlines()
    lines = defaultdict(list)
    timing = TimingReport(SCORE_START, SCORE_END, detailed=True)
    success = int(row["sender_exit"]) == 0
    raw = raw_path.read_bytes() if raw_path.exists() else b""
    for entry in raw.splitlines():
        tag, *fields = entry.split()
        lines[tag].append(fields)
        if success and tag in (b"TIMING", b"CPU", b"FRAME", b"BEAT"):
            timing.accept(entry)
    result_rows = lines[b"RESULT"]
    result = result_rows[0] if len(result_rows) == 1 else None
    summary_rows = [json.loads(line) for line in messages if line.startswith("{")]
    outcome = {
        **row,
        "board_status": result[0].decode() if result and len(result) >= 2 else None,
        "published": int(result[1]) if result and len(result) >= 2 and result[1].isdigit() else None,
        "verified": False,
    }
    if raw_path.exists():
        outcome["raw_sha256"] = sha256(raw).hexdigest()
    if not success:
        if len(summary_rows) == 1:
            outcome.update(summary_rows[0])
        outcome["sender_message"] = messages[-1] if messages else None
        return outcome, None

    if not result or len(summary_rows) != 1:
        raise ValueError(f"{name}: missing result or summary on successful sender exit")
    flash_log = (directory / f"{name}.flash.log").read_text()
    if flash_log.count("Hash of data verified.") != 3:
        raise ValueError(f"{name}: missing bootloader, partition, or application flash verification")
    placements = [tuple(map(int, fields)) for fields in lines[b"PLACEMENT"]]
    if placements != [PLACEMENTS[config]]:
        raise ValueError(f"{name}: worker placement differs from the frozen build")
    verify_result(b"RESULT " + b" ".join(result), "normal", payload, FRAME_COUNT, measured=True)
    if result[0] != b"OK":
        raise ValueError(f"{name}: board reported {result[0]!r}")
    candidates = [tuple(map(int, fields)) for fields in lines[b"CANDIDATE"]]
    expected = [(lead, *item) for lead, sequence in enumerate(reference) for item in sequence]
    # The report preserves candidate order within each lead.
    candidates.sort(key=lambda item: item[0])
    if candidates != expected:
        raise ValueError(f"{name}: candidate sequence differs from the scalar replay")
    fused = [int(location) for location, in lines[b"FUSED"]]
    late, expected_fused = offline_fusion(reference, FRAME_COUNT, 13, 128)
    fusion = (13, 128, late, 0)
    if fused != expected_fused or [tuple(map(int, fields)) for fields in lines[b"FUSION"]] != [fusion]:
        raise ValueError(f"{name}: fused sequence or settings differ from the frozen replay")
    measured = timing.summarize(b"RESULT " + b" ".join(result), fused, fusion, config)
    logged = summary_rows[0]
    if any(logged.get(field) != value for field, value in measured.items()):
        raise ValueError(f"{name}: raw timing disagrees with the sender summary")
    complete_frames = timing.frames[SCORE_START:]
    latencies = [
        max(worker1, worker2) - 1 - (index * 1_000_000 // 257)
        for index, _, worker1, worker2 in complete_frames
    ]
    if len(latencies) != SCORE_END - SCORE_START:
        raise ValueError(f"{name}: scored frames are missing")
    rank = (95 * len(latencies) + 99) // 100
    if sorted(latencies)[rank - 1] != measured["frame_latency"]["p95_us"]:
        raise ValueError(f"{name}: p95 differs from the scored frame timestamps")
    if sum(map(int, lines[b"CPU"][0])) != measured["pipeline_cpu_us"]:
        raise ValueError(f"{name}: task CPU counters disagree")
    outcome.update(logged)
    outcome.update(verified=True, candidates=len(candidates), fused_beats=len(fused))
    return outcome, (candidates, fused)


def binomial_sign_test(values):
    positive = sum(value > 0 for value in values)
    negative = sum(value < 0 for value in values)
    n = positive + negative
    probability = min(1, 2 * sum(comb(n, k) for k in range(min(positive, negative) + 1)) / 2**n)
    return {"positive": positive, "negative": negative, "non_tied": n,
            "tied": len(values) - n, "p_two_sided": probability}


def analyze_pairs(blocks):
    contrasts = {}
    for contrast in CONTRASTS:
        treatment, control = contrast
        pairs = []
        by_patient = defaultdict(list)
        for (record, repetition), block in sorted(blocks.items()):
            if treatment not in block or control not in block:
                continue
            run, baseline = block[treatment], block[control]
            pair = {
                "record": record, "repetition": int(repetition),
                "p95_delta_us": run["frame_latency"]["p95_us"] - baseline["frame_latency"]["p95_us"],
                "cpu_delta_us": run["pipeline_cpu_us"] - baseline["pipeline_cpu_us"],
            }
            pairs.append(pair)
            by_patient[record].append(pair)
        patient_effects = {}
        for patient, patient_pairs in sorted(by_patient.items()):
            latencies = [pair["p95_delta_us"] for pair in patient_pairs]
            costs = [pair["cpu_delta_us"] / SECONDS / 1000 for pair in patient_pairs]
            patient_effects[patient] = {
                "pairs": len(patient_pairs),
                "p95_us_median": median(latencies), "p95_us_range": [min(latencies), max(latencies)],
                "cpu_ms_per_s_median": median(costs), "cpu_ms_per_s_range": [min(costs), max(costs)],
            }
        contrasts[contrast] = {"pairs": pairs, "patient_effects": patient_effects}
        for metric in ("p95_us", "cpu_ms_per_s"):
            values = [effect[f"{metric}_median"] for effect in patient_effects.values()]
            if values:
                contrasts[contrast][f"{metric}_median"] = median(values)
                contrasts[contrast][f"{metric}_patient_range"] = [min(values), max(values)]
        if contrast == "BA":
            contrasts[contrast]["sign_test"] = binomial_sign_test([
                effect["p95_us_median"] for effect in patient_effects.values()
            ])
    return contrasts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    parser.add_argument("--manifest", type=Path, default=Path("docs/evaluation-manifest.md"))
    parser.add_argument("--inputs", type=Path, default=Path("data/replay/evaluation"))
    parser.add_argument("--scalar-replay", type=Path, default=Path("data/timing/issue9/scalar_replay"))
    args = parser.parse_args()
    planned, observed = read_schedule(args.manifest, args.captures)
    references = {}
    for record, _, _ in planned:
        if record not in references:
            payload, path = load_payload(args.inputs / f"{record}_2827_11051_scale1024.json")
            references[record] = payload, offline_candidates(args.scalar_replay, path)
    runs = []
    sequences = {}
    blocks = defaultdict(dict)
    for row in observed:
        record = row["record"]
        outcome, sequence = verify_attempt(args.captures, row, *references[record])
        runs.append(outcome)
        if sequence is None:
            continue
        if sequences.setdefault(record, sequence) != sequence:
            raise ValueError(f"{record}: candidate or fused sequences differ across runs")
        blocks[record, row["repetition"]][row["configuration"]] = outcome
    result = {
        "planned_trials": len(planned), "observed_trials": len(observed), "runs": runs,
        "complete_agreement_checked_blocks": sum(len(block) == 4 for block in blocks.values()),
        "contrasts": analyze_pairs(blocks),
        "cpu_tasks": list(TASKS),
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
