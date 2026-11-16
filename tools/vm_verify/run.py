#!/usr/bin/env python3
"""Play scripted sessions of the real game on an engine built with the script VM's checks and report what they found.

An on-demand tool for work on the script VM (src/libs/core: compiler, DATA, ATTRIBUTES, the name codec); see README.md.
It drives tools/scene_sweep with an engine configured with -DSTORM_SCENE_SWEEP=ON -DSTORM_VM_VERIFY=ON: a new game with
its opening dialogue, then the session's scenes back to back in one engine. In such a build every fast path of the VM
also computes the plain answer and aborts at the first difference, so the session passes only when all of them agreed.

  python3 tools/vm_verify/run.py --game <game dir> --engine <build>/engine --out <dir> [--save FILE] [--only SCENE ...]
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

# A town walk, interiors, land combat, the sea, a sea battle, a boarding, the world map and a storm.
SESSION = ['loc:FortFrance_town', 'loc:FortFrance_tavern', 'loc:FortFrance_townhall', 'combat:*', 'sea:Martinique',
           'battle:*', 'boarding:*', 'map:world', 'storm:*', 'night:*_town']

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--game', type=Path, required=True)
parser.add_argument('--engine', type=Path, required=True, help='host engine built with STORM_SCENE_SWEEP and STORM_VM_VERIFY')
parser.add_argument('--out', type=Path, required=True, help='report directory (on the game\'s filesystem)')
parser.add_argument('--save', type=Path, help='also boot into this save and play the session from it')
parser.add_argument('--only', nargs='+', default=SESSION, help='scenes instead of the default session')
parser.add_argument('--settle', type=int, default=3000, help='ms every scene plays before the next one')
parser.add_argument('sweep', nargs=argparse.REMAINDER, help='after --: more arguments for tools/scene_sweep/run.py')
args = parser.parse_args()

sweep = Path(__file__).resolve().parents[1] / 'scene_sweep/run.py'
totals, problems = {}, []
for name, extra in [('newgame', [])] + ([('save', ['--save', str(args.save)])] if args.save else []):
    out = args.out / name
    shutil.rmtree(out, ignore_errors=True)
    command = [sys.executable, str(sweep), '--game', str(args.game), '--engine', str(args.engine), '--out', str(out),
               '--jobs', '1', '--single-session', '--keep-work', '--settle', str(args.settle), '--only', *args.only,
               *extra, *[item for item in args.sweep if item != '--']]
    # The sweep's own verdict on the scenes (game data defects, pictures) is not this tool's: the engines' checks are.
    subprocess.run(command, stdout=(args.out / f'{name}.sweep.log').open('w'), stderr=subprocess.STDOUT)
    logs = sorted(out.glob('work/shard-*/run-*/stdout.log'))
    if not logs:
        problems.append(f'{name}: no engine ran (see {args.out}/{name}.sweep.log)')
    for log in logs:
        text = log.read_text(errors='replace')
        for line in re.findall(r'^.*vm verify: MISMATCH.*$', text, re.M)[:5]:
            problems.append(f'{name}: {line.strip()}')
        report = re.findall(r'^vm verify: no mismatch; checks:(.*)$', text, re.M)
        if not report:
            problems.append(f'{name}: {log} has no "vm verify: no mismatch" line: the engine did not end normally')
            continue
        for key, count in re.findall(r'(\w+)=(\d+)', report[-1]):
            totals[key] = totals.get(key, 0) + int(count)
    scenes = re.findall(r'^\d+ loaded (\S+)', '\n'.join(p.read_text(errors='replace')
                                                         for p in out.glob('work/shard-*/run-*/control/events.log')), re.M)
    print(f'{name}: {len(scenes)} scenes played: {" ".join(scenes)}')
    if len(scenes) < 3:
        problems.append(f'{name}: only {len(scenes)} scenes loaded')
    shutil.rmtree(out / 'work', ignore_errors=True)

print('checks: ' + ' '.join(f'{key}={count}' for key, count in sorted(totals.items())))
if not totals or not all(totals.values()):
    problems.append('a kind of check never ran: ' + ' '.join(key for key, count in totals.items() if not count))
for problem in problems:
    print('FAIL ' + problem)
sys.exit(1 if problems else 0)
