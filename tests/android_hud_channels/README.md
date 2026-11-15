Run `python3 tests/android_hud_channels/run.py` (C++20 compiler with ASan/UBSan; `CXX` override supported);
`--bench` also prints the cost of one loop of an unchanged channel, old publisher against new (-O2).

The main loop asks every HUD state channel of the portable UI every iteration (`android_ui.hpp`). A channel
compares the typed state its JSON is built from (`HudChannel`, `platform/android_hud_channel.hpp`, keyed on
the state class's `publicationKey()`) and builds and publishes its JSON only when that changed and the JSON
differs from the last one sent. `check.cpp` drives each channel's real state class through a deterministic
random walk of its mutations, new UI hosts included, and runs the old publisher (build the JSON every loop,
compare the text) beside the new one: both must publish the same byte-identical sequence, and a loop whose
typed state did not change must not build the JSON. `run.py` checks that the production publishers use the
keys the check drives. `ship` keeps its own publication (100 ms throttle, forced changes) and returns before
building when its key is the one last published; `vitals` compares the typed `LandVitals`
(`platform/android_land_vitals.hpp`), and the check also proves its JSON byte-identical to the old
concatenation. Not a device check.
