Run `python3 tests/relief_tile_policy/run.py` for CPU-only ASAN/UBSAN controls of the production recovery policy
and exact first-output ownership helper. The temporary binary is removed on exit. No GPU/model runtime is loaded.

The fixture covers valid active epochs, once-per-network/generation admission despite renewed deadlines,
revocation/expiry before and after the probe, cancellation, calibration/measurement validity, dimensions,
candidate bounds and exact first-output reuse once after moving into the ordinary tiling output buffer.
Mismatched shape/pointer falls back to inference; rejected output never becomes primed.

This does not prove ncnn output equivalence, real GPU timing, or a cold-loading speedup. Those need separate
exact-model GPU controls and cache/device-matched scene measurements, including rejected probe overhead.
