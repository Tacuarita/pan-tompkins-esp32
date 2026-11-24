# Evaluating multilead Pan–Tompkins on the ESP32-S3

This project studies how detector task placement and SIMD arithmetic affect real-time ECG processing on an ESP32-S3. It runs six independent fixed-point Pan–Tompkins detectors and combines their candidates into one sequence of estimated beat locations. The experiment measures frame processing latency and CPU cost while checking that every configuration produces the same detector and fusion outputs.

## Research questions

1. **Core placement.** Does placing two detector workers on separate cores reduce 95th-percentile frame processing latency, and how does that placement affect CPU time?
2. **SIMD squaring.** Does vectorizing the squaring of three lead derivatives reduce the cost of that operation once lane packing and stores are included? Does any kernel-level saving carry through to the complete pipeline?
3. **Correctness and reliability.** Do all configurations process the same frames in order, produce identical beat candidates and fused beat locations, and finish each frame before its deadline?

## Experimental design

The host streams six synchronized leads (I, II, III, aVR, aVL, and aVF) from PhysioNet INCART over UART. The board releases the fixed-point frames at the source sampling rate of 257 Hz. Two workers process three leads each, maintaining independent detector state per lead. A shared fusion stage requires confirmation from at least two distinct leads. [Replay format and data preparation](docs/incart-replay.md)

| Build | Detector worker placement | Three-lead squaring |
| --- | --- | --- |
| A | Both workers on core 1 | Scalar |
| B | Worker 1 on core 1, worker 2 on core 0 | Scalar |
| C | Both workers on core 1 | SIMD |
| D | Worker 1 on core 1, worker 2 on core 0 | SIMD |

Replay and fusion run on core 0 in every build, so the placement comparison includes contention with those tasks. Every build uses the same input frames, lead assignment, detector arithmetic outside the squaring stage, and fusion rules. B versus A is the primary comparison for scalar worker placement. C versus A and D versus B measure the effect of SIMD at each placement. D versus A measures the combined change. [Firmware and instrumentation](docs/esp32-uart-replay.md)

After a development pilot, the study fixed its settings and ran the four builds on one 30-second scored excerpt from each of six held-out patients. Each build ran three times per excerpt, for 72 runs. Comparisons pair runs from the same patient and repetition. The primary analysis summarizes the paired differences at the patient level and uses an exact two-sided sign test. [Frozen protocol and run order](docs/evaluation-manifest.md)

## What the evaluation measures

- **Frame processing latency.** Time from scheduled frame release until both workers finish processing the frame and hand off any new candidates. Each run contributes one 95th percentile over its 7,710 scored frames.
- **Pipeline CPU cost.** Scheduled CPU time summed across replay, both detector workers, and fusion, expressed in milliseconds per second of ECG.
- **Real-time reliability.** Frame deadline misses, backlog, UART underruns, ring-buffer overflows, and incomplete trials. A frame must complete before the next scheduled release, about 3.89 ms later.
- **Output agreement.** Both workers must process the canonical frames in order. Per-lead candidate tuples and fused beat locations must match the host scalar reference and agree across builds.
- **Detection quality.** A separate ten-minute host pilot scores fused beats against INCART annotations with one-to-one matching within 150 ms. It reports sensitivity and positive predictive value for the development record. [Annotation scoring method](docs/incart-scoring.md)

## Results

With scalar squaring, spreading the workers across both cores reduced p95 frame latency for all six patients. The median patient-level reduction was 78 µs, and pipeline CPU cost fell by 1.131 ms per second of ECG. SIMD used 5.3–5.6% fewer cycles in the squaring benchmark. In the full pipeline, SIMD increased p95 latency and CPU cost on one core. On two cores, its median paired p95 change was zero and CPU cost fell by 0.040 ms per second of ECG. [Paired results and variability](docs/evaluation-results.md)

All 72 held-out runs passed frame, candidate, and fusion agreement checks with zero frame deadline misses. The separate host pilot measured 98.94% sensitivity and 76.78% positive predictive value on development record I01. The performance findings apply to the instrumented six-lead ESP32-S3 workload, and the accuracy figures describe that single development record. [Hardware results](docs/evaluation-results.md#timing-and-reliability) · [Host pilot](docs/incart-scoring.md#reproduce-the-i01-pilot)

## Reproducing the study

- [Prepare the INCART replay files](docs/incart-replay.md).
- [Build and run the ESP32-S3 firmware](docs/esp32-uart-replay.md) using the [frozen evaluation settings](docs/evaluation-manifest.md).
- [Analyze the captured trials](docs/evaluation-results.md#evidence) or [reproduce the host annotation-scoring pilot](docs/incart-scoring.md#reproduce-the-i01-pilot).
