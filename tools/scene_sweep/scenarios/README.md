# Test scenarios

Named game states for testing screens a new game does not reach (gambling, port office, shipyard repair, boarding,
the journal with many quests, ...). Test tooling only: original GPL code, never shipped, never part of the game.

Each `<name>.c` here is a small script of the game's language that includes `scenario.c` (the driver) and makes its
state from a **start save** with the game's own functions (`AddPassenger`, `SetCompanionIndex`, `GenerateShipExt`,
`AddQuestRecord`, `QuestToSeaLogin_*`, `DoQuestReloadToLocation`, ...). It runs through the game's own migration hook:
`scenarios.py` writes `PROGRAM/migrations/NNNN_scenario_<name>.c` (the next free number; `migrations.c` runs every
migration a save has not seen while it loads) plus `PROGRAM/scenarios/` into a throw-away copy of the game. Loading
the start save then runs:

1. `Scenario_Setup()` inside the load: money, items, officers, companions, ship and damage, quests, people at their
   places, and the destination (`Scenario_Place` a location, `Scenario_PlaceSea` the sea off an island);
2. once the loaded scene is on screen the driver takes the hero there with a quest reload or a quest sea login, so the
   place is entered as the player enters it (its people, shops, doors, the enemies at sea);
3. `Scenario_Arrived()` on arrival, then after 3 s on screen the game is saved as **`scenario-<name>`** in the start
   save's profile (the game's `SaveEngineState`), then `Scenario_Trigger()` (run-only actions, not saved);
4. the segment unloads itself. Progress is traced as `Scenario <name>: ...` (system.log, logcat `StormEngine`), a
   failure as `Scenario <name>: FAILED <reason>`.

Later runs load `scenario-<name>` directly. **The saves contain game data**: keep them only in an ignored local folder
(the workspace uses `local-testing/scenarios/saves/`), never commit or publish them.

```
python3 tools/scene_sweep/scenarios.py list
# emulator (started with -read-only: the injected files go away with its overlay)
python3 tools/scene_sweep/scenarios.py device NAME -s emulator-5932 --start <start save> --saves <dir>
#   then load the start save on the device (it is the newest record: title -> «Продолжить»)
# host: the scene sweep's scenario:<name> scene (run.py --scenario), one engine per scenario
python3 tools/scene_sweep/scenarios.py sweep NAME... --game <game> --engine <sweep engine> --save <start save> \
    --out <report dir> --saves <dir> [run.py options]
```

The start save must be made on land (`local-testing/boot-stage/saint-pierre-streets`, the boot stage's save: the
hero in the streets of Сен-Пьер, Martinique, early game, opening quest lock already lifted). `Scenario_Unlock()` lifts
the opening quest's lock the way the game does (Mishelle.c) for saves made before that.

| Scenario | State | Screens it opens to |
| --- | --- | --- |
| `gamblers` | Сен-Пьер tavern at noon, 50 000 pesos; a card player at table 1, a dice player at table 2 | tavern dialogue, cards, dice, rest (self-talk) |
| `church` | Сен-Пьер church at noon, 20 000 pesos | priest dialogue, donation |
| `port_office` | Сен-Пьер port office, 200 000 pesos, two companion ships | port master dialogue, leaving/taking back a ship |
| `shipyard_repair` | Сен-Пьер shipyard, 200 000 pesos, a brig at hull 45 %, 4 cannons lost | shipyard, repair, cannons, sails and coat of arms |
| `fleet` | Сен-Пьер streets, corvette, navigator, boatswain, fighter, two loaded companions; the run then opens the month's pay (not saved) | ship screen, ship exchange, cargo/crew transfer, salary |
| `sea_prize` | the open sea, corvette and brig, a pirate war tartane and lugger attacking; the run then boards the tartane (not saved) | sea battle, boarding, plunder, prize captain, transfer |
| `maps` | Сен-Пьер streets, the best map, the atlas with six island maps, both halves of a treasure map | best map, atlas, map reading |
| `questbook` | Сен-Пьер streets, fourteen real quests in the journal, five completed | journal, completed quests |

A new scenario: copy one, keep its `// Scenario <name>: <state>` line (`list` prints it) and define the three hooks.
