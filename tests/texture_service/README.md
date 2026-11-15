# Loading cache admission

Run `nice -n 10 python3 tests/texture_service/run.py` in an authorized CPU slot.
The fixture executes production `texture/core/service.cpp` under ASan/UBSan,
with its actual worker count and queues. Only the external codec/cache boundary
is replaced by deterministic gated work; no models, GPU or renderer is started.

It occupies every worker, queues ordinary transcodes and cache tasks, promotes
the existing cache tickets, then releases one worker. The first two completions
must be cached maps, followed by an ordinary transcode. Further cached maps and
ordinary work must both progress. Cancel/reveal and new-generation controls must
restore ordinary admission for old tickets. Shutdown must drop unrun tasks and
join the occupied worker pool.

This establishes a bounded admission improvement, not Android loading duration,
disk throughput or GPU completion. Running codec work is not preemptible.
The game keeps the same worker count, queue/byte limits and two upscale uploads
per frame. The fixture compiler and binary use a temporary directory removed on exit.
