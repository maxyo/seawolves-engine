Run `python3 tests/android_hud_invalidate/run.py` (C++20 compiler with ASan/UBSan; `CXX` override supported).

`check.cpp` drives `LandRefreshGate` (`platform/android_land_commands.hpp`): the land menu is re-read on the
land interface's own "list changed" flag, a readiness or land-interface change, or after `force()`, and
never otherwise. Not a device check.
