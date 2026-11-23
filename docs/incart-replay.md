# INCART replay

From the repository root, run the pilot with Python 3.11 or newer. The generator uses the standard library.

```sh
python3 tools/incart_replay.py
python3 tools/incart_replay.py --offline
```

The first run downloads the release checksum list and the selected record's `.hea`, `.dat`, and `.atr` files to `data/incartdb/1.0.0/`. Subsequent runs reuse files whose SHA-256 matches the release list. Use `--offline` to require a complete, verified cache with no network access. Use `--record I02`, `--start`, and `--stop` to select a different half-open source sample interval. Use `--cache-dir` and `--output-dir` to override the default directories. [PhysioNet INCART v1.0.0](https://physionet.org/content/incartdb/1.0.0/)

## Format for the UART sender and host detector

- Send or read the `.bin` payload starting at byte zero. It contains consecutive 12-byte frames with six signed little-endian 16-bit samples in the order I, II, III, aVR, aVL, aVF. The frame layout is Python `struct` format `<6h`. The payload has no header or UART framing. Release frames at 257 Hz.
- Frame `i` in the payload corresponds to source sample `source_start_sample + i` in the JSON manifest. Keep the cached `.atr` file on the host. Its sample positions are relative to the original record, so use the same source offset when comparing detector output to annotations.
- The generator selects each lead by name and computes millivolts as `(digital_sample - baseline) / adc_gain`, using the header's ADC zero when the baseline is omitted. It multiplies by `counts_per_mV`, rounds to the nearest integer with ties away from zero, then saturates to `[-32768, 32767]`. The calculation uses exact integer ratios. A missing WFDB input sample stops generation. The manifest counts saturation separately for each lead. [WFDB header format](https://physionet.org/physiotools/wag/header-5.htm) · [WFDB signal format](https://physionet.org/physiotools/wag/signal-5.htm)
- The pilot uses 1,024 counts per mV, so one count represents 1/1,024 mV. Use `--scale` to try another positive integer scale during pilot tuning. Freeze the scale before evaluation.

## Pilot verification

The pilot uses record `I01`, patient 1, with source frames `[0, 7710)`. Reserve that patient for development when selecting held-out excerpts. The payload has 7,710 frames (30 seconds), 92,520 bytes, and zero clipped samples on all six leads.

| Host file | Path |
| --- | --- |
| UART and detector input | `data/replay/I01_0_7710_scale1024.bin` |
| Manifest | `data/replay/I01_0_7710_scale1024.json` |
| Reference annotations | `data/incartdb/1.0.0/I01.atr` |

Verify offline regeneration and the pilot payload checksum with these commands from the repository root.

```sh
replay_verify_dir=$(mktemp -d)
python3 tools/incart_replay.py --offline --output-dir "$replay_verify_dir"
cmp data/replay/I01_0_7710_scale1024.bin "$replay_verify_dir/I01_0_7710_scale1024.bin"
cmp data/replay/I01_0_7710_scale1024.json "$replay_verify_dir/I01_0_7710_scale1024.json"
sha256sum data/replay/I01_0_7710_scale1024.bin
```

Both comparisons passed. The expected SHA-256 is `4b8eb0394a64a53919661449a4cbfd60e50a710cc76c9d86ae9247a8e207e1a6`. Python WFDB 4.3.1 independently matched all 46,260 calibrated and quantized values and read 2,757 annotation events, with 47 in the pilot window. A nonzero-start check at a higher scale matched independent saturation counts and values. The command rejected a corrupted offline cache and an out-of-range excerpt. A fresh fetch and offline rerun of five `I02` frames in a temporary cache also produced identical outputs.
