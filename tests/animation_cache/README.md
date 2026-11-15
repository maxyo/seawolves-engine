# Animation data cache

The host test compiles all production animation sources. Only engine timing,
controls and file routing are replaced; the real AN loader reads a generated
one-bone fixture in the build directory. No game corpus or graphics device is
required. ASan and UBSan are enabled by default.

`python3 tools/ci/run_host_tests.py --only animation_cache` runs it; directly, with the pinned GLM 1.0.3
checkout:

```sh
python3 tests/animation_cache/run.py --build-dir build/animation-cache --glm-source <glm-checkout>
```

Coverage: case-insensitive shared lookup, one-frame reuse, 1ms frames, exact
30-second unscaled-time expiry with paused/scaled game time, reacquire/release
without a frame, multiple active references, trim before the first idle frame,
reload after trim, repeated trim, empty slots and saturating idle age.

The engine's LowMemory event dispatch is checked separately from this test;
this test exercises its `AnimationService::TrimCaches()` endpoint directly.

The idle budget uses `GetRDeltaTime()`, independent of game time scaling. The
engine caps that delta at 100ms per frame, so expiry can take longer than 30
wall-clock seconds across long stalls or suspension. Memory-pressure trimming
does not wait for that budget.
