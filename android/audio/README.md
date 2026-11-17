# Audio backend (miniaudio)

The miniaudio backend (`service_backend.cpp`) is the only SoundService implementation on Android and the
Linux host. It opens an explicit real-device backend list (Android AAudio/OpenSL ES; no null fallback) or
renders offline PCM for tests. A device that refuses to start or stop is logged
(`Audio device did not start: ...`) and the mixer stays silent until the next activation.

## Sources and licences

`tests/audio/run.py` downloads the sources into the ignored build directory and verifies their SHA256:

- miniaudio 0.11.23 (`mackron/miniaudio` tag), SHA256
  `7e4f3f13c8fe66df2080ac3dd12a89193e3c2463cb7f067c798abd7331cd8ee6`; use its MIT-0 alternative.
- stb_vorbis (`nothings/stb` commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`), SHA256
  `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`; use its MIT alternative.

Distributed binaries retain both notices and `tools/android/patches/miniaudio-0.11.23-error-cleanup.patch`,
which is applied to a build copy only. Vorbis floor-0 and chained streams are unsupported.

## Tests

```
python3 tests/audio/run.py
python3 tests/audio/run.py --build-dir build/audio-asan --sanitize
python3 tests/audio/run.py --build-dir build/audio-android \
  --cxx /path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++ \
  --compile-only
```

Prerequisites: C++17 compiler, Python 3, ffmpeg with libmp3lame/libvorbis. Fixtures are synthesized; no game
assets are used. `audio-probe <fixture-directory> --device` (`tests/audio/probe.cpp`) opens the real audio
output; on Android deploy the ARM64 binary, fixtures and matching `libc++_shared.so` and set
`LD_LIBRARY_PATH`.

## engine.ini `[sound]`

- `cache_mb` (4..512, default 48): LRU budget of decoded samples; samples in use are never evicted.
- `stream_threshold_mb` (1..64, default 4): decoded samples longer than this are streamed.
- `async_decode` (default 1): decode on a worker thread; `0` decodes in `SoundPlay` on the game thread.
