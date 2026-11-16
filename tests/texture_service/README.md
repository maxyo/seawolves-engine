# Texture Service scheduler

`python3 tests/texture_service/run.py` compiles the production `texture/core/service.cpp` with `check.cpp` under
ASan/UBSan and runs it with the real worker count and queues. Only the external codec/cache boundary is replaced by
gated work; no models, GPU or renderer start.

- Cache IO runs on its own worker: it completes while every transcode worker is held.
- IO fairness (two foreground, then one ordinary), the 256-task bound, cancelled tickets, the ready-bytes budget and
  generation revocation on a new loading scope.
- Shutdown drops queued work and joins an in-flight IO task without restarting CPU workers.
- The IO worker's pick classifies each task exactly once while the game thread changes its generation: a second
  thread flips a lone ticket between the current and an old generation during 5000 picks. The two-scan selection
  of owner build 1650 fails within a few hundred picks (`STORM_TEXTURE_SERVICE_SOURCE=<old service.cpp>` shows it).

Every wait is on progress, never on a clock, so a loaded machine changes how long it takes, not the result (about
1 s idle). A step that cannot finish is a deadlock: `run.py` stops it after `DEADLOCK_GUARD` seconds and names
the last step the check printed.
