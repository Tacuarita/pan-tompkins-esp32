# ESP32-S3 UART replay

Build with ESP-IDF 5.5.1 and prepare the canonical I01 pilot. Connect the board's USB-to-UART bridge. The firmware uses UART0 on GPIO43 (TX) and GPIO44 (RX) at 115200 baud, 8N1, with flow control disabled.

```sh
python3 tools/incart_replay.py --offline
idf.py -C targets/esp32 build
idf.py -C targets/esp32 -p /dev/ttyUSB0 flash
python3 tools/uart_replay.py /dev/ttyUSB0 --mode all
```

The Linux sender verifies the manifest and payload SHA-256 before opening the serial port. Use `--manifest` for a different canonical excerpt. The default mode runs a normal replay. `--mode all` runs the normal trial, then the two deliberate abort trials on the same connection. [Canonical replay format](incart-replay.md)

## Pacing and ownership

The board grants the sender up to 128 frames to prefill its 4 KiB UART receive buffer. Every 32 published frames it grants up to 32 more, bounded by the excerpt length. Each grant transfers consecutive raw 12-byte frames. The board schedules index `i` at `start + floor(i * 1,000,000 / 257)` microseconds on its monotonic clock. Each deadline is calculated from the same start time. The UART and replay task run on core 0. [ESP-IDF UART driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/uart.html)

Replay decodes the six signed little-endian samples and publishes the entire frame to the 64-slot shared ring. Worker 1 reads I, II, and III. Worker 2 reads aVR, aVL, and aVF. Each worker receives a read-only slot at its own cursor and advances the cursor after checking the index, updating all three scalar detector states, retaining any candidates, and computing a CRC-32 of its triplet bytes. The producer checks both cursors before it can reuse the oldest slot.

At a release deadline, fewer than 12 input bytes cause `INPUT_UNDERRUN`. A complete input frame with no writable ring slot causes `RING_OVERFLOW` before the slot can be modified. In the deliberate underrun trial, the board grants one frame and then runs out of input at index 1. In the deliberate overflow trial, the board prefills 65 complete frames and holds the second cursor at zero. Index 64 reaches a full ring and publication stops. A prefill timeout is reported separately. Reset the board before retrying an unexpected serial interruption.

## Reading the verification results

Each `RESULT` line contains the status, published count, scheduled span, observed span, and maximum lateness in microseconds, followed by each worker's frame count, index-error count, and hexadecimal CRC-32. Spans start at the first release and end at the last release attempt, including the failed attempt in an abort trial. The sender verifies all three outcomes, both worker reports against the canonical input, and the scheduled and observed timing. It exits with a nonzero status on any mismatch.

The host ring regression exercises slot retention while the other cursor advances, reuse after both readers finish, and concurrent reading of 7,710 synthetic frames through repeated ring wraps.

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Itargets/esp32/main \
  tests/replay_ring_test.c targets/esp32/main/replay_ring.c -o /tmp/replay_ring_test
/tmp/replay_ring_test
```

## I01 transport-only hardware evidence

On 2026-11-23, an ESP32-S3 revision v0.2 with a CP2102N USB UART bridge passed all three modes in one serial session. The I01 excerpt contained 7,710 frames and the verified payload SHA-256 was `4b8eb0394a64a53919661449a4cbfd60e50a710cc76c9d86ae9247a8e207e1a6`. ESP-IDF 5.5.1 built the tested application image with SHA-256 `edd4c83150217b2d8bf3cca11d387a0ef85558a579167028ba3763eba1ebe9b9`.

```text
normal RESULT OK 7710 29996108 29996145 85 7710 0 94341788 7710 0 e291ca01
underrun RESULT INPUT_UNDERRUN 1 3891 3924 47 1 0 35380840 1 0 19c20839
overflow RESULT RING_OVERFLOW 64 249027 249064 61 64 0 163d8865 0 0 00000000
```

Both worker CRCs matched the host's CRCs for all 7,710 frames and both workers reported zero index errors. The scheduled first-to-last span was 29.996108 s, the observed span was 29.996145 s, and the largest publication lateness was 85 µs. The host ring regression passed with undefined-behavior traps enabled. A separate cold-reset check verified that the sender can connect while the ESP32-S3 is still starting.

## Scalar A/B parity

Each worker owns three independent [scalar detector states](scalar-detector.md). Worker 1 runs on core 1 in both builds. Set `SECOND_WORKER_CORE` to 1 for A (the default) and 0 for B. Both workers run at priority 5 in both builds. The parity trial reports the observed replay and worker cores. It exports candidates after draining the ring. Before the run, the firmware allocates space for the maximum number of candidates based on the excerpt length and the 51-frame minimum distance between accepted beat locations on a lead.

From the repository root, activate ESP-IDF 5.5.1 and prepare the cached canonical replay. Flash and test the builds one at a time on the same board.

```sh
(
set -e
run_dir=$(mktemp -d)
python3 tools/incart_replay.py --offline
cc -std=c11 -O2 -Wall -Wextra -Wconversion -Wsign-conversion -pedantic -Werror \
  -Isrc src/pan_tompkins.c tools/scalar_replay.c -o "$run_dir/scalar_replay"

for config in A B; do
  core=1
  if [ "$config" = B ]; then core=0; fi
  idf.py -C targets/esp32 -B "$run_dir/build-$config" \
    -D SDKCONFIG="$run_dir/sdkconfig-$config" -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -D SECOND_WORKER_CORE="$core" build
  idf.py -C targets/esp32 -B "$run_dir/build-$config" -p /dev/ttyUSB0 flash
  python3 tools/uart_replay.py /dev/ttyUSB0 --configuration "$config" --mode all \
    --scalar-replay "$run_dir/scalar_replay"
done
cmp "$run_dir/sdkconfig-A" "$run_dir/sdkconfig-B"
)
```

For the normal trial, the sender verifies the core placement, both workers' frame counts and order, and both triplet CRCs against the canonical payload. It compares every board candidate's beat location and production frame with the host detector's ordered list for that lead, including searchback candidates. The remaining trials exercise input underrun and ring overflow.

### I01 scalar hardware results

On 2026-11-23, both builds passed on the same physical ESP32-S3 revision v0.2 using the 7,710-frame I01 excerpt above. The generated SDK configurations were byte-for-byte identical. The ESP-IDF 5.5.1 application SHA-256 was `b71ab796302af4a09737bc1985468f5dcdcbff0e58cab58e083aa1fdbde0e0fe` for A and `5c85820ec84ca9eb04e8aa6dd931470dd1d5a2fab956912fa5127434e5c1ea24` for B.

The placement tuple lists the replay task, worker 1, and worker 2 in that order. Each build processed all frames in order on both workers and matched all 275 offline candidates across the six leads.

```text
normal RESULT OK 7710 29996108 29996118 78 7710 0 94341788 7710 0 e291ca01
configuration A placement (0, 1, 1), exact candidate parity (I 43, II 53, III 38, aVR 46, aVL 49, aVF 46)
underrun RESULT INPUT_UNDERRUN 1 3891 3925 49 1 0 35380840 1 0 19c20839
overflow RESULT RING_OVERFLOW 64 249027 249063 61 64 0 163d8865 0 0 00000000

normal RESULT OK 7710 29996108 29996112 84 7710 0 94341788 7710 0 e291ca01
configuration B placement (0, 1, 0), exact candidate parity (I 43, II 53, III 38, aVR 46, aVL 49, aVF 46)
underrun RESULT INPUT_UNDERRUN 1 3891 3924 48 1 0 35380840 1 0 19c20839
overflow RESULT RING_OVERFLOW 64 249027 249063 61 64 0 163d8865 0 0 00000000
```

The host's scalar stage and decision tests and concurrent ring regression also passed with GCC undefined-behavior traps enabled. The offline Lead I replay matched the committed fixture. The changed firmware translation unit and its detector dependency are C. Both ESP32 builds exported compilation databases, and the repository's clang-tidy requirement for C++ changes did not apply.

## Two-lead fusion

The fusion task runs on core 0 at priority 4. Each worker appends every candidate to its lead's parity log before advancing its frame cursor. Fusion takes the smaller of the two cursors and considers candidates handed off through that common input frame. It counts candidates with production delay greater than `H` and excludes those candidates from grouping.

Fusion selects the earliest unassigned eligible location `a`. Once both workers have completed `a + W + H` inclusive, it consumes the eligible candidates in `[a, a + W]` and retains a beat at `a` if two distinct leads contributed. At the end of input it processes the final real worker progress. To score a full window, supply at least `W + H` real post-roll frames. [Fusion decision](adr/0002-finalize-fusion-by-input-frame.md)

The firmware uses provisional pilot settings of `W = 13` samples (about 51 ms) and `H = 128` samples (about 498 ms). On I01, 262 of the 275 scalar candidates are within that production-delay limit. Freeze the settings during the pilot before scoring held-out recordings.

### Verification

Run the host boundary regression from the repository root.

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -fsanitize=undefined \
  -fsanitize-undefined-trap-on-error -Isrc -Itargets/esp32/main \
  tests/beat_fusion_test.c src/beat_fusion.c targets/esp32/main/replay_ring.c \
  -o /tmp/beat_fusion_test
/tmp/beat_fusion_test
```

The regression covers delay boundaries `H` and `H + 1`, group boundaries `W` and `W + 1`, distinct-lead confirmation, one-time assignment, late arrival of an earlier location, the inclusive finalization frame, and either worker trailing the other. It also checks zero-width groups and cutoff arithmetic near the maximum frame index.

Use the scalar A/B build and flash commands above, and add `--verify-fusion --delay-fusion-handoffs` to the UART sender command for each build. The sender checks every per-lead candidate, including all late candidates, then checks the board's late count and exact fused sequence against an independent offline grouping of the reference candidates. The delayed replay inserts a 12 ms task delay on each candidate-producing frame of worker 2 before it advances its cursor. The sender checks the number of injected delays and compares output across the two runs. Compare the reported settings and sequence SHA-256 across placements as well.

Fusion verification extends the parity output with `FUSION <W> <H> <late-count> <delayed-handoffs>` followed by one `FUSED <location>` line per beat in ascending replay-relative frame order. The sender hashes the decimal locations as ASCII, one newline-terminated location per beat.

### I01 fusion hardware results

On 2026-11-23, both ESP-IDF 5.5.1 builds passed on the same ESP32-S3 revision v0.2 with byte-identical SDK configurations. In each run, both workers consumed all 7,710 canonical I01 frames in order, with triplet CRCs `94341788` and `e291ca01`. Every run matched all 275 offline candidates by lead, beat location, and production frame.

| Configuration | Worker cores | Injected delayed handoffs | Late candidates | Fused beats |
| --- | --- | ---: | ---: | ---: |
| A | 1, 1 | 0 | 13 | 66 |
| A | 1, 1 | 139 | 13 | 66 |
| B | 1, 0 | 0 | 13 | 66 |
| B | 1, 0 | 139 | 13 | 66 |

All four runs matched the offline fused sequence exactly. Its SHA-256 was `0eefd555f2c2e1946ef02c2359ff2e7096f4bf7e491f1de36028a3fb93cdc6ea`. Both builds also passed the deliberate input-underrun and ring-overflow trials. The prior flash contents were restored and verified byte-for-byte by readback.

The app image SHA-256 was `4a09a451cfdc890b7eefa5a59efc12badf60cb61f004ed0897deb4d7b4bd51ab` for A and `8aa0907a36400de1a9443c83f9070f3e2ba40abdaeee070d2cf18688532360e8` for B. Both builds exported compilation databases. This pass changed C and Python, so the C++ clang-tidy workflow did not apply.

## Scored timing and CPU measurements

Use the same firmware build options and placement assignments as the scalar parity run. Generate replay bytes with real pre-roll and at least `W + H` real post-roll frames. Score by replay-local index. This I01 pilot uses 257 pre-roll frames, 7,710 scored frames, and 257 post-roll frames.

```bash
run_dir=$(mktemp -d)
python3 tools/incart_replay.py --offline --stop 8224 --output-dir "$run_dir"
python3 tools/incart_replay.py --offline --stop 1028 --output-dir "$run_dir"
cc -std=c11 -O2 -Isrc src/pan_tompkins.c tools/scalar_replay.c -o "$run_dir/scalar_replay"
python3 tests/replay_timing_test.py

# With the board reserved and its current flash backed up, run once per flashed placement.
config=A
common=(python3 tools/uart_replay.py /dev/ttyUSB0 --configuration "$config"
    --scalar-replay "$run_dir/scalar_replay" --verify-fusion)
"${common[@]}" --manifest "$run_dir/I01_0_8224_scale1024.json" --score-window 257 7967 \
    --raw-report "$run_dir/full-$config.raw"
"${common[@]}" --manifest "$run_dir/I01_0_1028_scale1024.json" --score-window 64 675 \
    --delay-fusion-handoffs --raw-report "$run_dir/delay-$config.raw"
"${common[@]}" --manifest "$run_dir/I01_0_1028_scale1024.json" --score-window 64 799 \
    --compare-timing --raw-report "$run_dir/overhead-$config.raw"
for mode in underrun overflow; do
    python3 tools/uart_replay.py /dev/ttyUSB0 --manifest "$run_dir/I01_0_1028_scale1024.json" \
        --mode "$mode" --score-window 0 64 --raw-report "$run_dir/$mode-$config.raw"
done
```

The board records three 32-bit timestamps per retained frame in RAM. It timestamps publication immediately before the ring write, worker completion after detector updates and candidate handoff and just before advancing the cursor, and beat reporting at the fusion callback. Stamps are microseconds since the replay epoch plus one. Zero identifies a missing event. The board retains pre-roll frame records to measure any work outstanding at the start. It captures full completion times for scored frames even if they finish later, and captures beats by their estimated location even if fusion emits them after the window. Detailed records leave the board after replay and drain. At `[257,7967)`, frame timing records occupy 95,604 bytes plus 8 bytes per allocated beat slot.

The host derives full-frame latency from the later worker stamp minus scheduled release, and worker latency from each worker stamp minus scheduled release. The p95 uses nearest rank. A completion at the next scheduled release is a deadline miss. An unfinished frame whose deadline has elapsed at abort also counts as a miss, and a deadline still in the future is reported as unresolved. The host reconstructs backlog from publications and joint completions, grouping events at microsecond resolution. End backlog includes completions at the scheduled end and excludes new publications at that boundary. If a failed run stops before the boundary, end backlog and full-window CPU are unavailable. Underrun and overflow remain distinct trial outcomes.

Scheduler switch hooks accumulate actual scheduled execution for the replay task, both workers, and fusion. At each scheduled boundary the counters include the applicable portion of an in-flight task slice. Intervals are clipped to the window so a late timer wakeup cannot add work beyond it. CPU is reported in seconds per second of ECG using `(end - start) / 257` as the denominator. It uses scheduler task accounting, so interrupt work during a task's scheduled slice can be attributed to that slice. The matched detailed-on/off check keeps the CPU hooks, pacing, workload, and parity checks enabled in both runs. Its difference estimates the incremental cost of detailed frame and beat capture.

### I01 scored-window hardware results

ESP-IDF 5.5.1 builds A and B ran on the same physical ESP32-S3 with byte-identical SDK configurations. Both workers processed all 8,224 canonical frames in order. The triplet CRCs were `0f15ee60` and `83b1a809`, and the replay payload SHA-256 was `0d0b8420782fcfbf59fbac91130a241d72411f22b7db2ba764402500d94b906e`. Both builds matched all 298 offline candidates and emitted the same 70 fused beats. The window included 68 of those beats by location, including two reported after the scheduled end in each build.

| Metric | A | B |
| --- | ---: | ---: |
| Scored frames | 7,710 | 7,710 |
| Frame latency p95 (µs) | 250 | 170 |
| Worker 1 completion latency p95 (µs) | 246 | 166 |
| Worker 2 completion latency p95 (µs) | 244 | 168 |
| Beat reporting latency p95 (µs) | 548,925 | 548,842 |
| Input/replay CPU (ms per ECG second) | 12.849 | 14.831 |
| Worker 1 CPU (ms per ECG second) | 21.725 | 23.971 |
| Worker 2 CPU (ms per ECG second) | 21.610 | 23.907 |
| Fusion CPU (ms per ECG second) | 12.991 | 5.038 |
| Pipeline CPU (ms per ECG second) | 69.174 | 67.748 |
| Deadline misses / peak backlog / end backlog | 0 / 1 / 0 | 0 / 1 / 0 |
| Input underruns / ring overflows | 0 / 0 | 0 / 0 |

The injected-delay runs recorded 28 deadline misses in A and 27 in B. Peak backlog was 5 and end backlog was 1 in both. One scored frame finished after the window and one scored beat was reported after the window in each run. Both workers still processed all 1,028 frames exactly once. The last scheduled offset was 3,996,108 µs and maximum publication lateness was 93 µs in A and 406 µs in B. Both builds distinguished an input underrun after one published frame from overflow at 64 published frames. The overflow run retained 64 deadline misses, a peak backlog of 64, and an end backlog of 64.

The matched 735-frame window measured an incremental detailed capture cost of 2,499 µs in A (1.28% of baseline pipeline CPU) and 2,802 µs in B (1.48%). Each estimate uses one on/off pair with exact candidate and fusion parity.

Host timing tests, the fusion and ring regression tests, and an independent recomputation from the board records passed. The prior flash contents were restored and verified byte-for-byte by readback. Local board records and the independent checker are in `data/timing/issue6/`.

The app image SHA-256 was `43216eb31453ccc462e8bd8bd08a3746e0eb3290f64e3fd9e78b44fb59f2684e` for A and `ec3f299e2eb4a6d72d0aa18a3b95ee37598bd4da584595c2196126376afd54a4` for B. Both builds exported compilation databases. The implementation changed C and Python, so the C++ clang-tidy workflow did not apply.

## Three-lead squaring and A–D parity

All builds use the same detector triplet path. Each worker prepares three independent derivatives, squares the triplet, and feeds the three energies back into the corresponding detector states. A and B use scalar squaring. C and D use lanes 0–2 of the same aligned eight-lane vector, with zeros in lanes 3–7. Both kernels compute `d² >> 8` after the shared signed rounding and clipping step. Clipping to ±2,896 bounds the integration input to 0–32,761, within the signed 16-bit shifted result of `EE.VMUL.S16`. [ESP32-S3 technical reference manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)

The benchmark compares and times both production kernels on the board. In each pass, every clipped derivative appears once in each lead, with offset sequences to expose lane-order errors. Seven passes alternate which kernel runs first. Cycle counts include the shared call and loop, the three input loads and output stores, and the SIMD packing, shift-register handling, and vector load and store. Input generation and output comparison occur outside the counted intervals. The board checks exact scalar/SIMD agreement before accepting the benchmark.

Activate ESP-IDF 5.5.1, reserve the board, and back up the flash regions the four builds will overwrite. Use a fresh output directory for the reports. After the final run, restore the previous flash and verify it by readback.

```bash
run_dir=$(mktemp -d)
python3 tools/incart_replay.py --offline --stop 8224 --output-dir "$run_dir"
cc -std=c11 -O2 -Isrc src/pan_tompkins.c tools/scalar_replay.c -o "$run_dir/scalar_replay"

for config in A B C D; do
    case "$config" in A|C) core=1 ;; B|D) core=0 ;; esac
    case "$config" in A|B) simd=OFF ;; C|D) simd=ON ;; esac
    idf.py -C targets/esp32 -B "$run_dir/build-$config" \
        -D SDKCONFIG="$run_dir/sdkconfig-$config" -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -D SECOND_WORKER_CORE="$core" -D USE_SIMD_SQUARING="$simd" build
    idf.py -C targets/esp32 -B "$run_dir/build-$config" -p /dev/ttyUSB0 flash
    python3 tools/uart_replay.py /dev/ttyUSB0 --configuration "$config" \
        --scalar-replay "$run_dir/scalar_replay" --verify-fusion --benchmark-squaring \
        --manifest "$run_dir/I01_0_8224_scale1024.json" --score-window 257 7967 \
        --raw-report "$run_dir/full-$config.raw" | tee "$run_dir/full-$config.log"
done
```

### I01 hardware results

All four builds used the same ESP32-S3 at 160 MHz, byte-identical SDK configurations, and the same timing capture. Both workers processed all 8,224 frames in order, with zero index errors and triplet CRCs `0f15ee60` and `83b1a809`. The replay payload SHA-256 was `0d0b8420782fcfbf59fbac91130a241d72411f22b7db2ba764402500d94b906e`.

Each build matched the 298 offline candidates exactly on lead, beat location, and production frame. The separate fusion check matched all 70 fused beats, with 68 in the scored window and two of those emitted during post-roll. The fused sequence SHA-256 was `01091db5d4832b88dcad7adda2daa513afd8c4cb2b3d7b85c352891fc5831442`. All runs had zero deadline misses, input underruns, and ring overflows. Peak backlog was one frame and end backlog was zero.

| Metric | A | B | C | D |
| --- | ---: | ---: | ---: | ---: |
| Scalar cycles per triplet, median | 74.86 | 74.85 | 74.81 | 74.86 |
| SIMD cycles per triplet, median | 70.64 | 70.86 | 70.64 | 70.85 |
| Frame latency p95 (µs) | 250 | 172 | 254 | 172 |
| Worker 1 / 2 latency p95 (µs) | 247 / 247 | 167 / 170 | 251 / 248 | 167 / 170 |
| Worker 1 / 2 CPU (ms per ECG second) | 21.980 / 22.006 | 24.431 / 24.346 | 22.462 / 22.486 | 24.409 / 24.323 |
| Pipeline CPU (ms per ECG second) | 69.802 | 68.664 | 70.804 | 68.618 |

Each pass matched all 5,793 clipped derivative values in all three lanes. The full SIMD operation used 5.3–5.6% fewer cycles. In the single matched I01 pilot, C added 4 µs to p95 and 1.001 ms of pipeline CPU per ECG second relative to A. D had the same p95 as B and used 0.046 ms less pipeline CPU per ECG second. D reduced p95 by 78 µs and pipeline CPU by 1.184 ms per ECG second relative to A. All four runs are retained as equivalent work. These pipeline effects describe one replay per build on one excerpt.

The host scalar, ring, fusion, and timing regression checks passed. An independent check recomputed the board summaries from raw records and compared candidates and fused beats across A–D. The original flash was restored and verified byte-for-byte. Local records, build fingerprints, and readback checksums are in `data/timing/issue7/`. All four builds exported compilation databases. The changed source files are C and Python.
