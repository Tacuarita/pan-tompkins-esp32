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

The board grants the sender up to 128 frames to prefill its 4 KiB UART receive buffer. Every 32 published frames it grants up to 32 more, bounded by the excerpt length. Each grant transfers consecutive raw 12-byte frames. The board schedules index `i` at `start + floor(i * 1,000,000 / 257)` microseconds on its monotonic clock. Each deadline is calculated from the same start time. The UART and replay task run on core 0, and this pilot uses the same detector-worker placement as configuration A. [ESP-IDF UART driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/uart.html)

Replay decodes the six signed little-endian samples and publishes the entire frame to the 64-slot shared ring. Worker 1 reads I, II, and III. Worker 2 reads aVR, aVL, and aVF. Each worker receives a read-only slot at its own cursor and advances the cursor only after its last access. The producer checks both cursors before it can reuse the oldest slot. The workers in this transport pilot check frame order and compute a CRC-32 of their triplet bytes in frame order.

At a release deadline, fewer than 12 input bytes cause `INPUT_UNDERRUN`. A complete input frame with no writable ring slot causes `RING_OVERFLOW` before the slot can be modified. In the deliberate underrun trial, the board grants one frame and then runs out of input at index 1. In the deliberate overflow trial, the board prefills 65 complete frames and holds the second cursor at zero. Index 64 reaches a full ring and publication stops. A prefill timeout is reported separately. Reset the board before retrying an unexpected serial interruption.

## Reading the verification results

Each `RESULT` line contains the status, published count, scheduled span, observed span, and maximum lateness in microseconds, followed by each worker's frame count, index-error count, and hexadecimal CRC-32. Spans start at the first release and end at the last release attempt, including the failed attempt in an abort trial. The sender verifies all three outcomes, both worker reports against the canonical input, and the scheduled and observed timing. It exits with a nonzero status on any mismatch.

The host ring regression exercises slot retention while the other cursor advances, reuse after both readers finish, and concurrent reading of 7,710 synthetic frames through repeated ring wraps.

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Itargets/esp32/main \
  tests/replay_ring_test.c targets/esp32/main/replay_ring.c -o /tmp/replay_ring_test
/tmp/replay_ring_test
```

## I01 hardware evidence

On 2026-11-23, an ESP32-S3 revision v0.2 with a CP2102N USB UART bridge passed all three modes in one serial session. The I01 excerpt contained 7,710 frames and the verified payload SHA-256 was `4b8eb0394a64a53919661449a4cbfd60e50a710cc76c9d86ae9247a8e207e1a6`. ESP-IDF 5.5.1 built the tested application image with SHA-256 `edd4c83150217b2d8bf3cca11d387a0ef85558a579167028ba3763eba1ebe9b9`.

```text
normal RESULT OK 7710 29996108 29996145 85 7710 0 94341788 7710 0 e291ca01
underrun RESULT INPUT_UNDERRUN 1 3891 3924 47 1 0 35380840 1 0 19c20839
overflow RESULT RING_OVERFLOW 64 249027 249064 61 64 0 163d8865 0 0 00000000
```

Both worker CRCs matched the host's CRCs for all 7,710 frames and both workers reported zero index errors. The scheduled first-to-last span was 29.996108 s, the observed span was 29.996145 s, and the largest publication lateness was 85 µs. The host ring regression passed with undefined-behavior traps enabled. A separate cold-reset check verified that the sender can connect while the ESP32-S3 is still starting.
