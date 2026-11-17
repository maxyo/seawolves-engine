Run `python3 tests/android_hud_channels/run.py` (C++20 compiler with ASan/UBSan; `CXX` override supported);
`--bench` also prints the cost of one loop of an unchanged channel, old publisher against new (-O2).

A HUD state channel (`HudChannel`, `platform/android_hud_channel.hpp`) compares the typed state its JSON is
built from (the state class's `publicationKey()`) and builds and publishes its JSON only when that changed
and the JSON differs from the last one sent. `check.cpp` drives each channel's real state class through a
deterministic random walk of its mutations and runs the old publisher (build the JSON every loop, compare
the text) beside the new one: both must publish the same byte-identical sequence, and a loop whose typed
state did not change must not build the JSON. Not a device check.
