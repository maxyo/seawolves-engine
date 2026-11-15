#!/usr/bin/env python3
"""HUD state channels: JSON built only on a typed change, publications identical to the text compare.

`run.py --bench` also builds an optimized binary and prints the cost of one unchanged loop per
channel, old publisher against new.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
ui = (root / 'src/libs/util/include/platform/android_ui.hpp').read_text()


def publisher(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# Each publisher compares the typed key the check drives, and builds the JSON only inside publish.
for function, channel, key in (
        ('inline void publishSeaOrders()', 'seaOrdersChannel', 'seaOrders.publicationKey()'),
        ('inline void publishSeaQuickAction()', 'seaQuickActionChannel', 'seaQuickAction.publicationKey()'),
        ('inline void publishLandCommands()', 'landCommandsChannel', 'landCommands.publicationKey()'),
):
    body = publisher(ui, function)
    assert re.search(re.escape(channel) + r'\.publish\(' + re.escape(key) + r', \[', body), body
    assert 'json() ==' not in body and '== json' not in body, body

combat = (root / 'src/apps/engine/src/android_combat_state.hpp').read_text()
body = publisher(combat, 'inline void publishCombatState()')
assert re.search(r'combatChannel\.publish\(combatPublicationKey\(pistol\), \[', body), body
assert 'lastCombatState' not in combat, 'the text compare is gone'
key = publisher(combat, 'inline auto combatPublicationKey(')
for part in ('combatInput.publicationKey()', 'combatInteractKind', 'combatInteractName', 'combatInteractDialog', 'pistol'):
    assert part in key, (part, key)
body = publisher(ui, 'inline void publishGameplayState(const GameplayState &state)')
assert re.search(r'gameplayChannel\.publish\(state\.publicationKey\(\), \[', body), body
main = (root / 'src/apps/engine/src/main.cpp').read_text()
loop = main[main.index('STORM_PROFILE_ZONE("HUD adapter: gameplay, combat, map, ship");'):]
assert 'publishGameplayState(gameplayState);' in loop[:loop.index('publishWorldMapHud(')], 'the loop publishes the typed state'

with tempfile.TemporaryDirectory(prefix='storm-hud-channels-') as directory:
    compiler = os.environ.get('CXX', 'c++')
    include = '-I' + str(root / 'src/libs/util/include')
    binary = Path(directory) / 'check'
    subprocess.run([compiler, '-std=c++20', '-fsanitize=address,undefined', include, str(here / 'check.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    if '--bench' in sys.argv[1:]:
        bench = Path(directory) / 'bench'
        subprocess.run([compiler, '-std=c++20', '-O2', '-DNDEBUG', include, str(here / 'check.cpp'), '-o', str(bench)],
                       check=True)
        subprocess.run([str(bench), '--bench'], check=True)
