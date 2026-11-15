Run `python3 tests/bounce_field_cache/run.py` (C++20 compiler, ASan/UBSan; no renderer/GPU).

The fixture uses production `LampsOnSurfels`, `GatherBounce` and `EncodeBounceBlocks` to check exact restored
float fields and final bytes. It covers content-key mutations (including compound members and alpha cuts),
corruption/truncation/shape/size rejection, warm lookups skipping compute, bounded namespace replacement and
cancellation before/during publication. It does not measure device speed or exercise the effects job lifecycle.

`bounce-lamps-v1` is a separate disposable namespace, at most four 8 MiB completed entries and one 8 MiB temporary.
There is no resident field LRU. The active field is the existing bounce field; IO may temporarily hold its encoded
bytes and a decoded field (each bounded by 8 MiB). Oversized fields and unavailable cache storage use normal baking.
Only static location interiors are eligible; sun and transient sea/ship geometry are excluded. The actual surfel/ray
scene, world occluder geometry/UVs/alpha cuts and full lamp transport inputs form the key, independent of addresses,
scene names and layout counters. The method version must change with transport/falloff/occlusion/filtering semantics.
The existing texture and bounce-scene caches are neither re-keyed nor removed.

Runtime acceptance: same exact APK, preset and scene/cache fixture; cold miss then re-enter unchanged interior.
Require a field-cache hit, unchanged ready/resident resource criteria and rendered light, compare ready wall times;
modify one lamp/material/cut/geometry input and require a miss. Confirm cancel/scene replacement cannot publish an old
job into the current scene. The historical 4.762 s lamp stage motivates this work but is not its measured benefit.
Lamp RGB is keyed exactly, including flicker if Execute already ran; such different snapshots intentionally miss.
The stock loading transaction's resource-only Realize path leaves newly created lights at authored RGB until its
first ordinary Execute. Reusing a matching field there does not require replacing current colors with steady ones.
