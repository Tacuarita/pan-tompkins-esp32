# Pan–Tompkins Concurrency

This experiment replays six synchronized PhysioNet INCART ECG leads at 257 Hz on an ESP32-S3. It compares scalar and SIMD Pan–Tompkins detection with workers on one or two cores. Each configuration consumes the same fixed-point frames and uses the same beat fusion rules. We measure frame latency and pipeline CPU time and check detector and fusion agreement across configurations.

## Repository layout

- `docs/` holds the experiment specification, design decisions, and replay instructions.
- `GLOSSARY.md` defines the detector, fusion, and timing terms.
- `tools/` is for host-side replay preparation and verification.
- `data/` is for local source records, host annotations, and generated replay files.
