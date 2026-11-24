# Held-out ESP32-S3 concurrency results

All 72 scheduled runs passed timing, frame coverage, candidate, and fusion verification in the frozen order. The matrix covered 555,120 scored frames in 18 complete four-way blocks. The first sender started at 2026-11-23T23:59:33Z and the last finished at 2026-11-24T01:16:10Z.

## Paired effects

Each run contributes the nearest-rank p95 from its 7,710 scored frames and the four task CPU deltas summed and divided by 30 ECG seconds. Each contrast subtracts the control from the treatment in the same patient and repetition. The overall effect is the median of the patient medians, and the range shows variation across patients.

| Contrast | Pairs | Patients | Frame p95 difference (ms) | Patient range (ms) | CPU difference (ms per ECG s) | Patient range (ms per ECG s) |
| --- | --- | --- | --- | --- | --- | --- |
| B−A | 18 | 6 | -0.0780 | -0.0810 to -0.0770 | -1.131 | -1.184 to -1.068 |
| C−A | 18 | 6 | +0.0035 | 0.0030 to 0.0040 | +0.961 | 0.949 to 0.988 |
| D−B | 18 | 6 | +0.0000 | -0.0010 to 0.0000 | -0.040 | -0.045 to -0.033 |
| D−A | 18 | 6 | -0.0780 | -0.0810 to -0.0780 | -1.170 | -1.228 to -1.105 |

The B−A median p95 was lower for all six patients. The exact two-sided paired sign test used six non-tied patient medians and returned p = 0.03125.

Under independent sampling of patient effects from a continuous distribution, the B−A patient range gives a distribution-free 96.875% interval for the median (coverage 1 − 2/2⁶). The frozen evaluation set used six specified INCART patients.

## Variation within each excerpt

Each cell reports the median and the minimum to maximum over the matched repetitions.

| Contrast | Record | Pairs | Frame p95 difference (ms) | CPU difference (ms per ECG s) |
| --- | --- | --- | --- | --- |
| B−A | I03 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.077 [-1.080 to -1.072] |
| B−A | I06 | 3 | -0.0770 [-0.0780 to -0.0770] | -1.174 [-1.178 to -1.167] |
| B−A | I08 | 3 | -0.0780 [-0.0790 to -0.0780] | -1.184 [-1.188 to -1.180] |
| B−A | I09 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.118 [-1.121 to -1.104] |
| B−A | I12 | 3 | -0.0810 [-0.0810 to -0.0810] | -1.068 [-1.080 to -1.057] |
| B−A | I15 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.145 [-1.146 to -1.138] |
| C−A | I03 | 3 | +0.0040 [0.0040 to 0.0040] | +0.986 [0.980 to 0.987] |
| C−A | I06 | 3 | +0.0040 [0.0040 to 0.0040] | +0.963 [0.953 to 0.963] |
| C−A | I08 | 3 | +0.0040 [0.0030 to 0.0040] | +0.988 [0.985 to 0.991] |
| C−A | I09 | 3 | +0.0030 [0.0030 to 0.0030] | +0.960 [0.960 to 0.963] |
| C−A | I12 | 3 | +0.0030 [0.0030 to 0.0030] | +0.949 [0.939 to 0.956] |
| C−A | I15 | 3 | +0.0030 [0.0030 to 0.0030] | +0.958 [0.953 to 0.966] |
| D−B | I03 | 3 | +0.0000 [0.0000 to 0.0000] | -0.045 [-0.047 to -0.030] |
| D−B | I06 | 3 | -0.0010 [-0.0010 to 0.0000] | -0.039 [-0.045 to -0.029] |
| D−B | I08 | 3 | +0.0000 [0.0000 to 0.0000] | -0.040 [-0.057 to -0.039] |
| D−B | I09 | 3 | +0.0000 [0.0000 to 0.0000] | -0.040 [-0.050 to -0.028] |
| D−B | I12 | 3 | +0.0000 [0.0000 to 0.0000] | -0.033 [-0.053 to -0.025] |
| D−B | I15 | 3 | +0.0000 [0.0000 to 0.0000] | -0.041 [-0.057 to -0.035] |
| D−A | I03 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.117 [-1.124 to -1.110] |
| D−A | I06 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.211 [-1.213 to -1.207] |
| D−A | I08 | 3 | -0.0780 [-0.0790 to -0.0780] | -1.228 [-1.238 to -1.223] |
| D−A | I09 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.155 [-1.158 to -1.149] |
| D−A | I12 | 3 | -0.0810 [-0.0810 to -0.0810] | -1.105 [-1.110 to -1.101] |
| D−A | I15 | 3 | -0.0780 [-0.0780 to -0.0780] | -1.185 [-1.195 to -1.181] |

## Timing and reliability

Timing and CPU columns show medians over the verified runs of each build.

| Build | Verified runs | Frame p95 (µs) | Worker 1 / 2 p95 (µs) | Worker 1 / 2 CPU (ms per ECG s) | Pipeline CPU (ms per ECG s) | Beat reporting p95 (µs) |
| --- | --- | --- | --- | --- | --- | --- |
| A | 18 | 247.5 | 246 / 245 | 21.814 / 21.597 | 69.273 | 548926 |
| B | 18 | 170 | 165 / 167.5 | 24.196 / 24.108 | 68.164 | 548846 |
| C | 18 | 251 | 250 / 248 | 22.302 / 22.069 | 70.235 | 548930 |
| D | 18 | 169.5 | 165 / 167.5 | 24.173 / 24.093 | 68.121 | 548844 |

| Build | Attempts | Unverified or incomplete | Deadline misses | Unresolved deadlines | UART underruns | Ring overflows | Peak / end backlog (max frames) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 18 | 0 | 0 | 0 | 0 | 0 | 1 / 0 |
| B | 18 | 0 | 0 | 0 | 0 | 0 | 1 / 0 |
| C | 18 | 0 | 0 | 0 | 0 | 0 | 1 / 0 |
| D | 18 | 0 | 0 | 0 | 0 | 0 | 1 / 0 |

All scored frames completed before the scored window ended. Forty-eight scored beats were emitted at or after the end and are included in beat reporting latency. The matrix used the frozen images and excerpts throughout, with zero replacement attempts.

## Agreement

For each verified run, both workers processed all 8,224 frames in order, with zero index errors and triplet CRCs matching the canonical excerpt. Raw detailed timing covers the pre-roll and every scored frame. Board candidate tuples matched exactly on lead, location, and production frame. Fused location sequences matched both the host scalar reference and all other verified runs of the same excerpt.

| Record | Verified runs | Candidate tuples per run | Fused beats per run | Scored fused beats per run |
| --- | --- | --- | --- | --- |
| I03 | 12 | 273 | 47 | 43 |
| I06 | 12 | 252 | 43 | 40 |
| I08 | 12 | 191 | 33 | 32 |
| I09 | 12 | 267 | 46 | 42 |
| I12 | 12 | 271 | 46 | 43 |
| I15 | 12 | 274 | 47 | 43 |

The separate [annotation-based validation](incart-scoring.md) used the host fixed-point scalar detector and host fusion on development record I01 over source samples [3084, 157284), with one-to-one matching within 38 samples (150 ms tolerance). It found 929 TP, 10 FN, and 281 FP, for 98.94% sensitivity and 76.78% positive predictive value.

The board kernel benchmark measured 5.3–5.6% fewer cycles with SIMD for three-lead squaring, including packing and stores. In the full pipeline, SIMD increased p95 and CPU cost on one core. On two cores, the median p95 change was zero and the measured CPU saving was 0.040 ms per ECG second. [Board kernel benchmark](esp32-uart-replay.md#three-lead-squaring-and-ad-parity)

All held-out runs used detailed instrumentation. The earlier A/B on-off check measured an added pipeline CPU cost of 1.28% in A and 1.48% in B on one short matched run per build. [Instrumentation check](esp32-uart-replay.md)

## Evidence

Local evidence under `data/timing/evaluation/` includes the capture runner, planned order, attempted-trial ledger, flash logs, raw UART reports, sender summaries, and checksums. The host analyzer checks the schedule, reconstructs summaries, and reports the run-level and paired data in JSON. The ESP32-S3's original flash was restored and verified byte-for-byte. Nine host Python regression tests passed. [Frozen manifest](evaluation-manifest.md) · [Host analyzer](../tools/evaluate_matrix.py)

```sh
python3 tools/evaluate_matrix.py data/timing/evaluation > data/timing/evaluation/analysis.json
```
