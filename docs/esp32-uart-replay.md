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
