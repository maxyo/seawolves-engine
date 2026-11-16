#!/usr/bin/env python3
"""Play scripted sessions of the real game on an engine built with the script VM's checks and report what they found.

An on-demand tool for work on the script VM (src/libs/core: compiler, DATA, ATTRIBUTES, the name codec); see README.md.
It drives tools/scene_sweep with an engine configured with -DSTORM_SCENE_SWEEP=ON -DSTORM_VM_VERIFY=ON: a new game with
its opening dialogue, then the session's scenes back to back in one engine. In such a build every fast path of the VM
also computes the plain answer and aborts at the first difference, so the session passes only when all of them agreed.

  python3 tools/vm_verify/run.py --game <game dir> --engine <build>/engine --out <dir> [--save FILE] [--only SCENE ...]

With --reference ENGINE (an engine without the change under test; both built with STORM_SCENE_SWEEP) it compares the
two engines instead: each boots, starts a new game and enters the first town (and with --save loads the save and plays
its scene), with the game's time and rand() made repeatable, and writes its whole script state (every global variable
and attribute tree and the name codec, exactly as a save stores them) at each step on the way. The states must be
equal byte for byte, the sweep script's own counters aside.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys

import state_diff

# A town walk, interiors, land combat, the sea, a sea battle, a boarding, the world map and a storm.
SESSION = ['loc:FortFrance_town', 'loc:FortFrance_tavern', 'loc:FortFrance_townhall', 'combat:*', 'sea:Martinique',
           'battle:*', 'boarding:*', 'map:world', 'storm:*', 'night:*_town']

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--game', type=Path, required=True)
parser.add_argument('--engine', type=Path, required=True, help='host engine built with STORM_SCENE_SWEEP and STORM_VM_VERIFY')
parser.add_argument('--out', type=Path, required=True, help='report directory (on the game\'s filesystem)')
parser.add_argument('--save', type=Path, help='also boot into this save and play the session from it')
parser.add_argument('--reference', type=Path, help='compare the script states of --engine with this engine\'s')
parser.add_argument('--only', nargs='+', default=SESSION, help='scenes instead of the default session')
parser.add_argument('--settle', type=int, default=3000, help='ms every scene plays before the next one')
parser.add_argument('--limit', type=int, default=1500, help='seconds one engine may take for its session')
parser.add_argument('sweep', nargs=argparse.REMAINDER, help='after --: more arguments for tools/scene_sweep/run.py')
args = parser.parse_args()

here = Path(__file__).resolve().parent
sweep = here.parent / 'scene_sweep/run.py'
args.out.mkdir(parents=True, exist_ok=True)

# The steps of a session up to which two runs of one engine give the same state: later ones depend on how many
# frames the background loading took (the save's scene after a second of play already differs between two runs of
# one engine: character positions in the sixth digit).
STATE_STEPS = {'newgame': ['loaded_menu_main', 'end_menu_main', 'loaded_newgame_intro', 'end_newgame_intro',
                           'loaded_loc_FortFrance_town'],
               'save': ['loaded_menu_main', 'end_menu_main', 'load', 'loaded_save_boot']}
SWEEP_VARIABLE = re.compile(r'^ss[A-Z]')  # tools/scene_sweep/scene_sweep.c counts its own frames and time


def run_sweep(command, log, environment=None):
    """The sweep with its engine, stopped after --limit seconds: a script that went wrong may wait for ever."""
    process = subprocess.Popen(command, env=environment, stdout=log.open('w'), stderr=subprocess.STDOUT,
                               start_new_session=True)
    try:
        process.wait(timeout=args.limit)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        print(f'FAIL the session did not end in {args.limit} s and was stopped (see {log})')


def play_states(engine, name, extra):
    """One engine through a session with its states written; {step: path}."""
    out = args.out / name
    shutil.rmtree(out, ignore_errors=True)
    (out / 'state').mkdir(parents=True)
    library = args.out / 'repeatable_rand.so'
    subprocess.run([os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-O2', '-o', str(library),
                    str(here / 'repeatable_rand.c')], check=True)
    # A fixed graphics preset: the automatic one follows the measured frame time, and the renderer writes the
    # preset's scene detail into script variables (iGrassQuality, InterfaceStates.SeaDetails).
    environment = dict(os.environ, STORM_VM_STATE_DIR=str((out / 'state').resolve()),
                       STORM_FX_OPTIONS=os.environ.get('STORM_FX_OPTIONS', 'fx_preset=2'),
                       LD_PRELOAD=':'.join(filter(None, (str(library.resolve()), os.environ.get('LD_PRELOAD')))))
    # The game's time runs 20 ms a frame, far ahead of the clock: the sweep's own time limits are in game time.
    command = [sys.executable, str(sweep), '--game', str(args.game), '--engine', str(engine), '--out', str(out / 'sweep'),
               '--jobs', '1', '--single-session', '--settle', '1000', '--timeout', '30000', '--only',
               'loc:FortFrance_town', *extra, *[item for item in args.sweep if item != '--']]
    run_sweep(command, out / 'sweep.log', environment)
    return {path.stem.split('-', 1)[1]: path for path in sorted((out / 'state').glob('*.state'))}


def compare_states():
    problems, compared = [], 0
    for session, extra in [('newgame', [])] + ([('save', ['--save', str(args.save)])] if args.save else []):
        reference = play_states(args.reference, f'{session}-reference', extra)
        candidate = play_states(args.engine, f'{session}-engine', extra)
        for step in STATE_STEPS[session]:
            if step not in reference or step not in candidate:
                problems.append(f'{session}: no state "{step}" from the {"reference" if step not in reference else "engine"} '
                                f'(see {args.out}/{session}-*/sweep.log)')
                continue
            size = reference[step].stat().st_size
            if reference[step].read_bytes() == candidate[step].read_bytes():
                print(f'{session} {step}: equal, {size} bytes')
                compared += 1
                continue
            a, b = state_diff.parse(reference[step]), state_diff.parse(candidate[step])
            for state in (a, b):
                state['values'] = {key: value for key, value in state['values'].items() if not SWEEP_VARIABLE.match(key)}
            lines = state_diff.differences(a, b, 30)
            if lines:
                problems.append(f'{session} {step}: the states differ (reference / engine):\n    ' + '\n    '.join(lines))
            else:
                print(f'{session} {step}: equal but for the sweep\'s own counters, {size} bytes')
                compared += 1
    print(f'{compared} states equal')
    for problem in problems:
        print('FAIL ' + problem)
    sys.exit(1 if problems else 0)


if args.reference:
    compare_states()

totals, problems = {}, []
for name, extra in [('newgame', [])] + ([('save', ['--save', str(args.save)])] if args.save else []):
    out = args.out / name
    shutil.rmtree(out, ignore_errors=True)
    command = [sys.executable, str(sweep), '--game', str(args.game), '--engine', str(args.engine), '--out', str(out),
               '--jobs', '1', '--single-session', '--keep-work', '--settle', str(args.settle), '--only', *args.only,
               *extra, *[item for item in args.sweep if item != '--']]
    # The sweep's own verdict on the scenes (game data defects, pictures) is not this tool's: the engines' checks are.
    run_sweep(command, args.out / f'{name}.sweep.log')
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
