# INCART beat scoring

`tools/score_incart.py` scores fused beats on the host from an INCART replay manifest and its cached `.atr` file. It checks the replay payload and annotation SHA-256 against the manifest. It shifts replay-relative fused locations by the manifest's source start sample before scoring against the reference locations. The reference includes WFDB QRS codes, including ventricular ectopic beats, irrespective of beat type. [INCART v1.0.0](https://physionet.org/content/incartdb/1.0.0/) · [WFDB QRS code mapping](https://github.com/bemoody/wfdb/blob/master/lib/ecgmap.h)

Both reference beats and estimates must fall in the half-open source sample window `[start, stop)` to participate. Replay must include at least `W + H` real samples after the scored window so groups anchored near the end can finish. The scorer sorts both streams and pairs the earliest compatible reference and estimate, consuming each event at most once. This maximizes the number of pairs within the fixed tolerance. At 257 Hz, 38 samples is within 150 ms and 39 samples exceeds it. Extra detections, including surplus duplicates, count as false positives. Unmatched reference beats count as false negatives. Sensitivity and positive predictive value are reported as fractions, with `null` for a zero denominator. [WFDB beat comparison tolerance](https://physionet.org/physiotools/wag/bxb-1.htm)

## Reproduce the I01 pilot

Run from the repository root with Python 3.11 or newer and WFDB 4.3.1 installed in the Python environment (`python -m pip install wfdb==4.3.1`). The commands use the [cached INCART files](incart-replay.md).

```sh
pilot_dir=data/scoring/issue8
mkdir -p "$pilot_dir"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc \
    src/pan_tompkins.c tools/scalar_replay.c -o "$pilot_dir/scalar_replay"
python tools/incart_replay.py --offline --record I01 --start 2570 --stop 157541 \
    --output-dir "$pilot_dir"
python tools/score_incart.py "$pilot_dir/I01_2570_157541_scale1024.json" \
    --scalar-replay "$pilot_dir/scalar_replay" --start 3084 --stop 157284 \
    --window 13 --max-delay 128 > "$pilot_dir/pilot-score.json"
python -m unittest discover -s tests -p 'score_incart_test.py' -v
```

The host ran the fixed-point scalar Pan–Tompkins detector on all six leads and grouped its candidates with `uart_replay.offline_fusion`, using `W = 13` and `H = 128` samples. The detector and fusion implementation are from commit `825c6b4`. At 1,024 counts/mV, the pilot replay covered source samples `[2570, 157541)`. The scored window `[3084, 157284)` covers 12 seconds through 612 seconds exclusive, with 514 pre-roll and 257 post-roll samples. I01 is a development record.

| Reference beats | Scored detections | TP | FN | FP | Sensitivity | Positive predictive value |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 939 | 1,210 | 929 | 10 | 281 | 98.94% | 76.78% |

The payload SHA-256 was `0e7adfcbe5b4c7575efff93223aed71c85d3c01d10473cb7063cbf853d6795b6`. The 1,218 replay-relative fused locations across the full replay had SHA-256 `b8183b0301abe37bb1c01634d0e73980c86cf7bf9c5ad36590d11a26a0898895` as newline-terminated decimal locations.

On the short 8,224-frame replay, the same host tools matched all 298 per-lead candidates, 13 late candidates, and 70 fused locations in each existing A–D board capture. The ten-minute replay ran on the host. A separate dynamic-programming calculation reproduced the pilot match count. The scoring tests exercise surplus duplicates, the inclusive start and exclusive stop on both streams, offsets of ±38 and ±39 samples, ambiguous pairing, and empty denominators.
