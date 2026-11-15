Run `python3 tests/android_hud_invalidate/run.py` (C++20 compiler with ASan/UBSan; `CXX` override supported).

The touch HUD asks the scripts for state only they compute when the original game would, never on a timer.
`check.cpp` drives `LandRefreshGate` (`platform/android_land_commands.hpp`): the land menu is re-read on the
land interface's own "list changed" flag, a readiness or land-interface change, or after `force()`, and
never otherwise. `run.py` checks the wiring in the production sources: `MSG_BATTLE_LAND_START` and
`BI_MSG_CARE_COMMANDLIST_UPDATE` reach the flag through `BIManSign::SetUpdate`, as do the menu opening or
closing, executed commands and the original cooldown steps; the main loop polls without a clock; a tap is
re-checked against a fresh read; the open menu's targets are not part of the closed menu's identity; and
the ship abilities are re-asked only when `procSetUsingAbility`'s inputs change. The sea orders follow the
same rule: `BI_IN_CREATE_SHIP`, `BI_IN_DELETE_SHIP`, `BI_IN_DEAD_SHIP` and `BI_MSG_REFRESH` reach the flag
through `BIShipIcon::SetUpdate`, as do the menu opening or closing and executed commands;
`BIShipIcon::ReadAndroidOrders` collects the companions again only on that update; `pollSeaOrders` and
`SeaOrdersRefreshGate` take no clock (the gate itself is driven by `tests/android_sea_orders`). Not a device
check.
