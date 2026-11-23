# Scalar Pan–Tompkins detector at 257 Hz

`src/pan_tompkins.c` is the C11 detector core for host replay and board integration. Zero-initialize one `PtDetector` per lead and call `pt_process` once for each consecutive sample. A successful call writes the estimated beat location and the frame on which the decision was made. Both indices are zero-based within the replay. Add the manifest's `source_start_sample` to get the original record index. Each state supports up to `UINT32_MAX` frames and uses fixed storage.

## Filter and numerical rules

The detector follows Pan–Tompkins with a bandpass, five-point derivative, squaring, moving-window integration, adaptive thresholds on integrated energy and filtered amplitude, T-wave discrimination, and searchback. The filter is redesigned for INCART's native rate. [Pan and Tompkins, 1985](https://doi.org/10.1109/TBME.1985.325532)

- The bandpass is a symmetric 129-tap FIR with a 64-frame group delay. For offsets `k = -64 … 64`, subtract the ideal 5 Hz low-pass from the ideal 15 Hz low-pass, using `sin(2πfk/257)/(πk)` and its limit `2f/257` at zero. Multiply by the Hamming window `0.54 + 0.46 cos(πk/64)` and normalize the centered response at 10 Hz to unity. Quantize to Q15 by rounding to nearest with ties away from zero, then adjust the center tap so the integer taps sum to zero. The quantized gain is 0.500 at 5 Hz, 1.002 at 10 Hz, 0.504 at 15 Hz, and 0.0024 at 50 Hz.
- The FIR uses zero-filled history and accumulates products in signed 32 bits. The sum of absolute integer tap values is 50,964. Even at full-scale input the absolute sum is bounded by `50964 × 32768`, with room for the rounding bias. Divide by 32,768, round to nearest with ties away from zero, and saturate to signed 16 bits. The output remains in replay counts.
- For bandpass output `y`, the derivative at frame `n` is `(2y[n] + y[n−1] − y[n−3] − 2y[n−4])/2`. The divisor gives four times the gain of the normalized five-point stencil with its sample-period factor absorbed into the adaptive levels. The delay is two frames. Round to nearest with ties away from zero, then clamp to `[-2896, 2896]`.
- Square the clipped derivative in signed 32 bits and shift its nonnegative result right by eight, rounding down. The largest output is 32,761, which fits a signed 16-bit SIMD result for later parity work. Sum the most recent 39 squared values in unsigned 32 bits. Detect local maxima on that sum and floor-divide by 39 for threshold comparisons. A plateau is confirmed on the first falling frame and uses the last frame of the plateau as its peak.
- Convert durations to samples by rounding `milliseconds × 257 / 1000` to nearest. The 150 ms integration window is 39 frames, the 200 ms refractory interval is 51 frames, and the 360 ms T-wave interval is 93 frames. Candidate decisions start at frame 171, when a local maximum can have full filter, derivative, and integration history. Replay ends at the last supplied frame. Supply real post-roll when decisions are needed near the end of a scored window.

## Candidate decisions

For an integration peak at frame `n−1` confirmed on frame `n`, find the greatest absolute bandpass amplitude and derivative magnitude over the 39 integrated frames. The bandpass amplitude for each derivative frame comes from two frames earlier. The earliest sample wins equal bandpass magnitudes. Subtract the 64-frame bandpass and two-frame derivative delays from the winning derivative frame to estimate the beat location. Report the current frame `n` as the production frame.

- Keep signal and noise levels separately for bandpass magnitude and integrated energy, initially zero. Update each relevant level as `(7 × old + peak + 4) / 8` with integer division. Accepted peaks update signal levels and rejected local maxima update noise levels. Each primary threshold is the noise level plus an upward-rounded quarter of the positive signal-minus-noise difference, with a minimum of one. A primary candidate must meet both thresholds.
- Require a location at least 51 frames after the previous estimated beat. If the gap is from 51 through 92 frames, reject the peak as a possible T wave when its maximum derivative magnitude is less than half that of the previous accepted peak.
- Retain the strongest eligible rejected peak by integrated energy if it reaches the upward-rounded half thresholds in both channels. A primary acceptance clears the retained peak. Once at least one RR interval exists, searchback can accept the retained peak if it still reaches the half thresholds. Its deadline is the last accepted location plus the nearest integer to 1.66 times the integer mean of up to eight recent accepted RR intervals, plus 105 frames to allow for the maximum primary decision delay. A primary decision takes precedence on the same frame. A searchback candidate keeps its earlier location and uses the actual later production frame.

## Reproduce the host reference

From the repository root, generate the replay as described in [INCART replay](incart-replay.md). With the cache present, the following commands regenerate it offline and compare the Lead I output byte for byte.

```sh
scalar_dir=$(mktemp -d)
python3 tools/incart_replay.py --offline --output-dir "$scalar_dir"
cc -std=c11 -O2 -Wall -Wextra -Wconversion -Wsign-conversion -pedantic -Werror \
    -Isrc src/pan_tompkins.c tools/scalar_replay.c -o "$scalar_dir/scalar_replay"
"$scalar_dir/scalar_replay" "$scalar_dir/I01_0_7710_scale1024.bin" I \
    > "$scalar_dir/lead_I_candidates.csv"
cmp tests/fixtures/I01_0_7710_scale1024_lead_I_candidates.csv "$scalar_dir/lead_I_candidates.csv"
sha256sum "$scalar_dir/I01_0_7710_scale1024.bin" "$scalar_dir/lead_I_candidates.csv"

cc -std=c11 -O2 -Wall -Wextra -Wconversion -Wsign-conversion -pedantic -Werror \
    -Isrc src/pan_tompkins.c tests/scalar_detector_test.c -lm -o "$scalar_dir/scalar_detector_test"
"$scalar_dir/scalar_detector_test"
```

The input SHA-256 is `4b8eb0394a64a53919661449a4cbfd60e50a710cc76c9d86ae9247a8e207e1a6`. The 7,710-frame replay produces [43 ordered candidates](../tests/fixtures/I01_0_7710_scale1024_lead_I_candidates.csv) with CSV SHA-256 `d88681f83afabc5dc399d8fa9eeec6bc3d2fb1dfeb90eece40ae57358e04feae`. The first candidate is `I,116,183` and the last is `I,7484,7551`. The file is a parity reference for this replay and detector, with no additional pre-roll or post-roll.

The host tests independently reconstruct the FIR and compare its outputs, the derivative, squaring, and the integration sum with direct calculations on 4,096 synthetic frames. The inputs exercise signed rounding ties, both saturation directions, steady DC, and the entire clipped derivative domain for squaring. Decision checks cover equality at the primary thresholds, plateau termination, the refractory and T-wave boundaries, polarity inversion, and a weak beat located at 1740 that is recovered by searchback on frame 1951.
