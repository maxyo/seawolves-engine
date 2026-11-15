#!/usr/bin/env python3
"""The engine boot stage of the gate: an ASan+UBSan host engine boots the game, loads a save and three scenes.

  tools/ci/boot_stage.py [--budget S] [--build DIR] [--out DIR] [--save FILE] [--game DIR] [--no-build]

What ships starts here as on a phone, through the seams the host checks replace with stubs (the script VM, the
entity lifetimes, the renderer, the techniques, the save loader): the scene sweep (tools/scene_sweep, one engine)
boots to the main menu, loads the save (scene save:boot: the town Сен-Пьер of the canonical save), then sails out
(sea:Martinique) and enters a tavern (loc:FortFrance_tavern, through the town again). It fails on a crash, a
sanitizer report (ASan stops the engine, UBSan lines fail the scene), a fatal engine error, script errors, missing
resources, a black or HUD-less frame, or the time budget. Three scenes from a save, not the all-scenes sweep:
tools/scene_sweep/README.md.

Inputs (each missing one fails the stage loudly, nothing is skipped):
- STORM_VIRTUALGL: the VirtualGL directory (--render gles-gpu: OpenGL ES on the host GPU, the phones' backend);
- STORM_GAME_DIR: the game (default <workspace>/local-testing/extracted/app);
- STORM_BOOT_SAVE: the canonical save (default <workspace>/local-testing/boot-stage/saint-pierre-streets, a copy
  of the Test Lab scenario 3 save «Сен-Пьер улицы»; the hero stands in the town of Martinique);
- STORM_CI_WORK: the shared dependency sources (as tools/ci/gate.py).

The build directory (default <worktree>/build/boot-asan, RelWithDebInfo, -fsanitize=address,undefined,
-DSTORM_SCENE_SWEEP=ON) is kept and rebuilt incrementally through ccache. Times are logged per step and per scene.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[2]
# The workspace holds local-testing/ (the game, the save) and storm-engine-android/ (the shared dependencies): the
# worktree's parent, or further up for a worktree inside local-testing.
workspace = next((p for p in root.parents if (p / 'local-testing').is_dir() and (p / 'storm-engine-android').is_dir()),
                 root.parent)
ci_work = Path(os.environ.get('STORM_CI_WORK', workspace / 'storm-engine-android/build/android-ci'))
SCENES = ('sea:Martinique', 'loc:FortFrance_tavern')  # after save:boot (the town); the save is on Martinique
SANITIZE = '-fsanitize=address,undefined -fno-omit-frame-pointer'
# halt_on_error: an ASan report ends the run (the sweep reports the crash with the report in the engine log).
# detect_leaks=0: the engine frees much of its state at exit by ending the process. verify_asan_link_order=0:
# VirtualGL's faker libraries are preloaded before the statically linked ASan runtime (they do not allocate first).
ASAN_OPTIONS = 'halt_on_error=1:detect_leaks=0:verify_asan_link_order=0:print_summary=1'
# ubsan.supp: the known reports, each with its cause; any other UBSan report fails the scene it was written in.
UBSAN_OPTIONS = f"print_stacktrace=1:report_error_type=1:suppressions={root / 'tools/ci/ubsan.supp'}"


def log(text):
    print(f'boot: {text}', flush=True)


def fail(text):
    sys.exit(f'BOOT STAGE FAILED: {text}')


def run(command, **kwargs):
    return subprocess.run([str(c) for c in command], cwd=root, **kwargs)


def build(directory):
    """Configures the sanitizer build once (again when this tree's dependency pins change) and builds the engine."""
    prepared = run([sys.executable, root / 'tools/android/engine_sources.py', '--shared', ci_work],
                   capture_output=True, text=True)
    if prepared.returncode:
        fail(f'the pinned sources could not be prepared in {ci_work}:\n{prepared.stdout}{prepared.stderr}')
    sources = prepared.stdout.strip()
    cache = directory / 'CMakeCache.txt'
    configured = re.search(r'^STORM_HOST_SOURCES:\w+=(.*)$', cache.read_text(), re.M) if cache.exists() else None
    if not configured or configured[1] != sources:
        launcher = root / 'tools/ccache/launcher'
        command = ['cmake', '-S', root, '-B', directory, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                   '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
                   f'-DCMAKE_C_COMPILER_LAUNCHER={launcher}', f'-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}',
                   f'-DCMAKE_C_FLAGS={SANITIZE}', f'-DCMAKE_CXX_FLAGS={SANITIZE}',
                   '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined',
                   '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined', '-DSTORM_SCENE_SWEEP=ON',
                   f'-DSTORM_HOST_SOURCES={sources}', f'-DSTORM_AUDIO_SOURCES={ci_work}/audio-host/patched',
                   f'-DSTORM_FFMPEG_CACHE={ci_work}/ffmpeg-cache', f'-DSTORM_FFMPEG_SOURCE={ci_work}/sources/ffmpeg',
                   '-DSTORM_OPEN_AUDIO=ON']
        if run(command).returncode:
            fail(f'configuring {directory} failed')
    for command in ([root / 'tools/ccache/warmth', 'begin', directory], ['ninja', '-C', directory, 'engine'],
                    [root / 'tools/ccache/warmth', 'end', directory]):
        if run(command).returncode:
            fail(f'{" ".join(map(str, command))} failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--budget', type=int, default=int(os.environ.get('STORM_BOOT_BUDGET', 300)),
                        help='seconds the sweep may take (the build is not counted; default 300, $STORM_BOOT_BUDGET)')
    parser.add_argument('--build', type=Path, default=root / 'build/boot-asan')
    parser.add_argument('--out', type=Path, default=root / 'build/gate/boot')
    parser.add_argument('--game', type=Path,
                        default=Path(os.environ.get('STORM_GAME_DIR', workspace / 'local-testing/extracted/app')))
    parser.add_argument('--save', type=Path, default=Path(os.environ.get(
        'STORM_BOOT_SAVE', workspace / 'local-testing/boot-stage/saint-pierre-streets')))
    parser.add_argument('--no-build', action='store_true', help='use the engine already built in --build')
    args = parser.parse_args()
    vgl = os.environ.get('STORM_VIRTUALGL')
    if not vgl or not all((Path(vgl) / 'usr/lib' / name).is_file() for name in ('libdlfaker.so', 'libvglfaker.so')):
        fail('STORM_VIRTUALGL is not set to a VirtualGL directory (usr/lib/libvglfaker.so): the boot stage renders '
             'with OpenGL ES on the host GPU through VirtualGL (tools/scene_sweep/README.md, Renderer)')
    if not args.game.is_dir():
        fail(f'the game is missing: {args.game} (STORM_GAME_DIR)')
    if not args.save.is_file():
        fail(f'the save is missing: {args.save} (STORM_BOOT_SAVE)')
    started = time.monotonic()
    if not args.no_build:
        build(args.build.resolve())
    built = time.monotonic()
    engine = args.build.resolve() / 'engine'
    if not engine.is_file():
        fail(f'no engine in {args.build}')
    log(f'build {built - started:.0f} s')
    args.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ASAN_OPTIONS=ASAN_OPTIONS, UBSAN_OPTIONS=UBSAN_OPTIONS)
    command = [sys.executable, root / 'tools/scene_sweep/run.py', '--game', args.game, '--engine', engine,
               '--out', args.out, '--save', args.save, '--only', *SCENES, '--jobs', '1', '--render', 'gles-gpu',
               '--virtualgl', vgl, '--timeout', str(min(240, args.budget)),
               # Compiled scripts kept between runs (rebuilt when the game's scripts change): 20 s of the ASan boot.
               '--script-cache', args.build.resolve() / 'script-cache']
    sweep = subprocess.Popen([str(c) for c in command], cwd=root, env=env, start_new_session=True)
    try:
        code = sweep.wait(timeout=args.budget)
    except subprocess.TimeoutExpired:
        os.killpg(sweep.pid, signal.SIGTERM)
        try:
            sweep.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(sweep.pid, signal.SIGKILL)
            sweep.wait()
        fail(f'over its time budget: the sweep took more than {args.budget} s (--budget, STORM_BOOT_BUDGET)')
    swept = time.monotonic()
    report = args.out / 'report.json'
    if report.exists():
        scenes = json.loads(report.read_text())['scenes']
        for name, entry in scenes.items():
            load = f", load {entry['load_ms'] / 1000:.1f} s" if 'load_ms' in entry else ''
            began = f", began {entry['begin_ms'] / 1000:.1f} s after the engine started" if 'begin_ms' in entry else ''
            log(f"{entry['status'].upper():5} {name}: {entry.get('seconds', '-')} s{load}{began}"
                + (f" ({'; '.join(entry['reasons'][:3])})" if entry['reasons'] else ''))
    log(f'sweep {swept - built:.0f} s (budget {args.budget} s), total {swept - started:.0f} s; report {report}')
    if code:
        fail(f'the sweep failed (exit {code}): {args.out}/summary.txt')
    log('passed')


if __name__ == '__main__':
    main()
