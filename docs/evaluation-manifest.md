# ESP32-S3 concurrency evaluation manifest

Frozen after the issue #9 pilot and before any held-out timing for issue #10.

## Patient split and replay

Development uses INCART v1.0.0 patient 1 (I01). Reserve patients 2–7 for the matrix. Patient numbers come from the checksum-verified headers. Select the lowest-numbered record for each of those patients and use the same source interval for every record. The pilot informed the settings using I01. The held-out preparation verifies source files and generates replay bytes. [INCART v1.0.0](https://physionet.org/content/incartdb/1.0.0/)

Every excerpt uses source interval `[2827, 11051)` and scored window `[3084, 10794)` (12 to 42 seconds). In replay-local coordinates the scored window is `[257, 7967)`. Supply 257 real pre-roll frames and 257 real post-roll frames. The post-roll exceeds the 141 frames required by `W + H`. Each payload is 98,688 bytes with 8,224 frames, including 7,710 scored frames. Preparation reported zero clipped samples on every lead in all six excerpts.

| Patient | Record | Replay payload SHA-256 | Reference annotations SHA-256 |
| ---: | --- | --- | --- |
| 2 | I03 | `38c3a8ec4669c0424f8cf28059d873f5003618880c5ba4969724f8def631a906` | `8c242de60d1b7c6b8297a9b06bd0758fbeff3a35b23e0be34c33918489c92967` |
| 3 | I06 | `c83a30152e0f118c88d46329e781626f23be34eff19ea221d2b539d8fc9ca63d` | `369c9ae509f88fdd373bdcfdccbfcf1c2873e5b4f704c0937d0c04e193f6d287` |
| 4 | I08 | `25486c4e8fc4e2acd3f8107ec0e8fa477fdb4e312758c1186e0cf16a55fcba45` | `7caba653d33e45628bf106a12d682edf8529ec1252489a9d9050054599998fae` |
| 5 | I09 | `440077f03af59681b2cb384cc5c8e83ddc7f9f8e7032c8501dd8051531e623fe` | `428895f5f28a042bd8f19bbf80dbfd7c4401c4d59606c52ec36bb547473b884a` |
| 6 | I12 | `7570467f054a7dfc8e9f3f58a6766c805de39917d0e6090603a7540c8667a5c4` | `dc7ecebae64e5a205501e51ad6ee8457cef7cdf5d2892f789a700f337fc12322` |
| 7 | I15 | `f133e033b002268b044a5db5c3bd02da89f1cd5a9152678ed1c6c95777c08a9c` | `cbcdda2d1bf92ec844dc858c21b2cefaa6deccf6875bac175bbb772849a38e15` |

Use the generated replay files and sidecar manifests in `data/replay/evaluation/`. The generator validates all three source files against the release checksums. Offline regeneration reproduced all six payloads and sidecars byte for byte.

```sh
for record in I03 I06 I08 I09 I12 I15; do
    python3 tools/incart_replay.py --offline --record "$record" \
        --start 2827 --stop 11051 --scale 1024 --output-dir data/replay/evaluation
done
```

## Firmware and numerical settings

- Replay at 257 Hz. Convert each lead using `(ADC - baseline) / gain` from the WFDB header, scale by 1,024 counts/mV, round nearest with ties away from zero, and saturate to signed 16 bits. Each 12-byte frame contains little-endian I, II, III, aVR, aVL, and aVF in that order. The short I01 pilot had zero clipped samples on every lead. [Replay format](incart-replay.md)
- Use the 129-tap symmetric Q15 FIR in `bandpass` at `b26a784`, with group delay 64 and nominal 5–15 Hz passband. Retain its signed rounding and saturation, five-point derivative clipped to ±2,896, `d² >> 8` squaring (0–32,761), 39-frame integration, 51-frame refractory period, 93-frame T-wave discrimination interval, and the documented integer adaptive thresholds and searchback. SIMD uses lanes 0–2 with the other five lanes zero. [Exact coefficients](../src/pan_tompkins.c) · [Detector numerical rules](scalar-detector.md)
- Use two-lead confirmation with `W = 13` and `H = 128` samples, including both endpoints of the location and production-delay bounds. Finalize by shared input-frame progress. The I01 ten-minute host pilot with those settings produced 929 TP, 10 FN, and 281 FP (98.94% sensitivity and 76.78% positive predictive value). Keep these pilot settings for the concurrency comparison. [Fusion decision](adr/0002-finalize-fusion-by-input-frame.md) · [Scoring result](incart-scoring.md)
- Worker 1 owns I, II, III on core 1. Worker 2 owns aVR, aVL, aVF on core 1 in A/C and core 0 in B/D. A/B use scalar squaring and C/D use SIMD. Both workers have priority 5 and 4,096-byte stacks. Replay runs in the ESP-IDF main task on core 0 at priority 1 with a 3,584-byte stack. Fusion runs on core 0 at priority 4 with a 4,096-byte stack. The release timer runs on core 0.
- Use ESP-IDF 5.5.1 on the dual-core ESP32-S3 at 160 MHz, with a 1,000 Hz FreeRTOS tick and external RAM disabled. UART0 uses 115200 baud, 8N1, no flow control, 4,096-byte RX and 1,024-byte TX buffers, 128 prefill frames, and 32-frame grants. Use the 64-frame shared ring and detailed timing enabled. The I01 transport and timing pilots support keeping these sizes. [Transport and instrumentation](esp32-uart-replay.md)

### Build identifiers

Reuse the pilot images in `data/timing/issue9/firmware/{A,B,C,D}/`. Each directory includes the matching bootloader, partition table, and `flash_args`. Verify all build artifacts from `data/timing/issue9/` with `sha256sum -c build-sha256.txt`. The builds use the `b26a784` source baseline plus the trial memory measurement in [replay_uart.c](../targets/esp32/main/replay_uart.c). The instrumented source file has SHA-256 `fc5c9d91fb6102be15333192b53aaa2c8524ca66ed4560997a5a183be7eb67fb`.

| Build | Worker 2 core | `USE_SIMD_SQUARING` | Application image SHA-256 |
| --- | ---: | --- | --- |
| A | 1 | OFF | `0cd50ec6cf3ee7543889895ad8783d8d9de5b0d1f565e4690b96a319c0d27406` |
| B | 0 | OFF | `340e5c82431ca72c5b02809e3214d01c765b1df2dad6ed8861f9f9456e54434c` |
| C | 1 | ON | `3c0a90bec408547f559263d13d8f1448346d36a5149a2e485fa76bc14c947a19` |
| D | 0 | ON | `84fd17f6527a60ebc660417c4ba49ee63e1689e62517d0e60ed91cfc2d748721` |

The generated SDK configuration was byte-identical in all builds, with SHA-256 `b781a71a4ce89cb4b947e1e04db6f02788b5e4808f4bfc41d6b89b5d1be01449`. The toolchain was Espressif Xtensa GCC 14.2.0 (esp-14.2.0_20241119). All four builds exported compilation databases. The changed code is C and Python.

## Pilot memory accounting

For a logging-enabled trial, measure minimum free internal 8-bit-capable heap from before trial allocation until replay and final fusion drain finish. ESP-IDF sums the individual heap regions' low-water marks, which gives a conservative bound when the regions reach their minima at different times. Read the four pipeline tasks' FreeRTOS stack high-water marks after drain and before deleting the tasks and exporting the logs. The marks report bytes of unused stack. [ESP-IDF heap accounting](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/system/mem_alloc.html) · [ESP-IDF task API](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/system/freertos_idf.html)

| Storage | Bytes at 8,224 frames | Allocation |
| --- | ---: | --- |
| UART RX and TX buffers | 5,120 | Driver payload buffers, installed before each trial's heap measurement |
| Shared replay ring | 1,036 | Static, including cursors |
| Two worker objects | 3,744 | Static, including six 600-byte detectors |
| Fusion and its trial bookkeeping | 88 | Replay task stack, including 64 bytes of fusion state |
| Replay, two workers, and fusion task stacks | 15,872 | Configured stack capacities, including the replay task created at boot |
| Per-lead candidate arrays | 7,776 | Heap, 162 records per lead at 8 bytes each |
| Fused locations | 1,944 | Heap, up to 486 locations |
| Frame and beat timing data | 99,492 | Heap, 7,967 frame records at 12 bytes plus 486 beat records at 8 bytes |

The four trial buffers total 109,212 bytes before allocator overhead. The heap watermark also reflects driver and runtime allocations such as UART bookkeeping, task control blocks, and the timer and semaphore.

### Instrumented A–D results

One trial per build ran on the same ESP32-S3 revision v0.2. The I01 replay covered source frames `[0, 8224)`, used the same replay-local scored window as evaluation, and had payload SHA-256 `0d0b8420782fcfbf59fbac91130a241d72411f22b7db2ba764402500d94b906e`.

| Build | Min. free internal heap (B) | Replay stack (B) | Worker 1 stack (B) | Worker 2 stack (B) | Fusion stack (B) | Peak backlog (frames) | Deadline misses | Frame latency p95 (µs) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A | 246,852 | 1,308 | 3,268 | 3,152 | 3,256 | 1 | 0 | 251 |
| B | 246,852 | 1,308 | 3,156 | 3,152 | 3,256 | 1 | 0 | 171 |
| C | 246,852 | 1,308 | 3,268 | 3,136 | 3,256 | 1 | 0 | 255 |
| D | 246,852 | 1,340 | 3,140 | 3,152 | 3,256 | 1 | 0 | 171 |

Each stack column reports minimum unused bytes. All four builds retained detailed timing for the full 7,710-frame scored window and processed all 8,224 input frames in order on both workers, with zero input underruns, ring overflows, or end-of-window backlog. Each matched the host and issue #7 on all 298 candidate tuples and 70 fused locations. Two scored beats in each build were emitted during post-roll. The full captures, independent recalculation, and image and capture checksums are in `data/timing/issue9/`. Nine Python tests and the host scalar, ring, and fusion regressions passed. The original flash was restored and verified byte-for-byte.

## Trial order and scoring

Use three repetitions (72 runs, 36 scored minutes). The full-window I01 baseline had zero deadline misses in all placements and at most one outstanding frame. Three repeats provide a within-excerpt spread while keeping the planned six independent patient effects for the sign test. Two more repeats would add 24 scored minutes and would improve the estimate of within-patient variation, with the same six patient-level observations. The pilot did not measure repeat-to-repeat variation.

Finish each repetition in ascending patient order. Use the following configuration order within each patient and repetition. The four sequences rotate build positions and each ordered adjacent pair occurs once across the four sequences.

| Record | Repetition 1 | Repetition 2 | Repetition 3 |
| --- | --- | --- | --- |
| I03 | A B D C | C D B A | B C A D |
| I06 | B C A D | D A C B | C D B A |
| I08 | C D B A | A B D C | D A C B |
| I09 | D A C B | B C A D | A B D C |
| I12 | A B D C | C D B A | B C A D |
| I15 | B C A D | D A C B | C D B A |

All runs use `--score-window 257 7967` with detailed capture, the same bytes per excerpt, offline scalar candidate verification, fusion parity, and a retained raw UART report. Compare ordered frame coverage, exact candidate tuples, and the fused sequence across builds. Calculate per-run nearest-rank p95 from scheduled release to later worker completion, and sum the four scheduled task CPU deltas clipped to the window, divided by 30 ECG seconds. Count completion at or after the next scheduled release as a deadline miss, and retain complete trials with misses. Score fused beat reporting latency by estimated location, including scored beats emitted during post-roll.

After backing up the board and flashing the matching saved image, run the sender from the repository root. Set `config`, `record`, and `repeat` according to the table.

```sh
mkdir -p data/timing/evaluation
python3 tools/uart_replay.py /dev/ttyUSB0 --configuration "$config" \
    --manifest "data/replay/evaluation/${record}_2827_11051_scale1024.json" \
    --scalar-replay data/timing/issue9/scalar_replay --verify-fusion \
    --score-window 257 7967 \
    --raw-report "data/timing/evaluation/${record}-${repeat}-${config}.raw" \
    > "data/timing/evaluation/${record}-${repeat}-${config}.log"
```

For B–A take the median of paired p95 differences within each patient, then report all six effects, their median, paired CPU effects, and the exact two-sided sign test excluding ties. Report C–A, D–B, and D–A effects and their spread. Include within-excerpt and across-patient variation, valid pair counts, and complete four-way block counts. Incomplete trials remain in the reliability evidence and are excluded from matched-work estimates. Valid pairs can contribute even when a different configuration in the block fails. A four-way comparison requires a complete, agreement-checked block.

Allow at most one replacement attempt per interrupted planned trial and only with independent evidence of an external interruption, such as a host sender crash or a disconnected serial cable logged by the host. Retain the original capture and the external evidence, then repeat the same configuration, excerpt, and repetition before proceeding with the block. Save replacement captures with a `-replacement` suffix before the file extension. A device deadline miss, UART underrun, ring overflow, or parity failure is insufficient on its own. Record any settings or implementation fix, rerun affected configurations, and use fresh patients for evaluation when any held-out excerpts informed tuning.

For separate annotation scoring, use the fixed WFDB QRS reference and one-to-one earliest-compatible matching within 150 ms (at most 38 samples). Both locations must be inside the declared half-open source window. Surplus duplicates count as false positives. Report TP, FN, FP, sensitivity, and positive predictive value, with the scoring window and fused-beat implementation identified. [Scoring rules](incart-scoring.md)
