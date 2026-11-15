#!/usr/bin/env python3
"""Check of tools/ci/affected.py, the gate's selection: the false negatives found by the architecture audit stay
fixed, and every source file of the engine selects at least one host or game-data check or the engine boot stage
(a change never selects nothing). Builds nothing; the build graphs are faked with a tiny Ninja directory."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/ci'))
sys.dont_write_bytecode = True
import affected  # noqa: E402

failures = []


def path(*parts):
    """A path this file does not spell: the check's own text would otherwise name it (affected.names)."""
    return '/'.join(parts)


def expect(condition, text):
    if not condition:
        failures.append(text)


def selected(plan, group):
    return {name for name, (run, _) in plan[group].items() if run}


with tempfile.TemporaryDirectory() as temp:
    none = Path(temp) / 'no-builds'  # no build graph anywhere: name rules only

    def plan(path, builds=none):
        return affected.plan({path: 'M'}, repo=root, builds=builds)

    # Run-time renderer input of no build: the boot stage.
    grass = plan(path('src', 'techniques', 'effects', 'grass.fx'))
    expect(grass['boot'][0] and 'run time' in grass['boot'][1], f'grass.fx does not select the boot stage: {grass["boot"]}')
    # Spelled without the final slash (CMake add_subdirectory, a Python path): the relief checks.
    relief = selected(plan(path('src', 'libs', 'relief', 'prepare.cpp')), 'host_checks')
    expect({'renderer_bgfx', 'relief_scheduler'} <= relief, f'relief/prepare.cpp selects {sorted(relief)}')
    # A library a game-data check links (its CMakeLists.txt), not only the headers check.cpp includes.
    far_lod = selected(plan(path('src', 'libs', 'geometry', 'far_lod', 'layout.cpp')), 'game_checks')
    expect('far_lod' in far_lod, f'far_lod/layout.cpp selects game checks {sorted(far_lod)}')
    # The boot stage's own tools.
    expect(plan(path('tools', 'scene_sweep', 'scene_sweep.c'))['boot'][0], 'scene_sweep.c does not select the boot stage')
    # A name is never the start of a longer one.
    spelled = Path(temp) / 'spelled'
    spelled.mkdir()
    (spelled / 'run.py').write_text(f"include = root/'{path('src', 'libs', 'relief_ui')}'\n"
                                    f"other = '../../{path('src', 'libs', 'sea')}\"'\n")
    expect(affected.names(spelled, {path('src', 'libs', 'relief', 'x.cpp'): 'M'}) is None, 'relief matched relief_ui')
    sea = path('src', 'libs', 'sea', 'y.cpp')
    expect(affected.names(spelled, {sea: 'M'}) == (path('src', 'libs', 'sea'), sea), 'a quoted directory was not matched')

    # With a boot build graph: a source file it does not read and no check selects is owned by no check: boot.
    fake = Path(temp) / 'builds'
    boot = fake / 'boot-asan'
    boot.mkdir(parents=True)
    read = root / path('src', 'libs', 'core', 'src', 'compiler.cpp')
    (boot / 'build.ninja').write_text(f'rule copy\n  command = cp $in $out\nbuild out: copy {read}\n')
    subprocess.run([shutil.which('ninja') or 'ninja', '-C', str(boot)], check=True, capture_output=True)
    if not (boot / '.ninja_deps').exists():
        (boot / '.ninja_deps').write_bytes(b'')
    affected.graph.cache_clear()
    in_graph = plan(path('src', 'libs', 'core', 'src', 'compiler.cpp'), fake)['boot']
    expect(in_graph[0] and 'boot build reads' in in_graph[1], f'compiler.cpp (in the boot graph): {in_graph}')

    # Every source file selects a check or the boot stage, with and without the boot build graph.
    tracked = subprocess.run(['git', '-C', str(root), 'ls-files', *affected.SOURCE_ROOTS], capture_output=True,
                             text=True, check=True).stdout.split()
    sources = [source for source in tracked if affected.is_source(source)]
    expect(len(sources) > 1000, f'only {len(sources)} source files found')
    boot_only = 0
    for builds in (none, fake):
        for source in sources:
            result = plan(source, builds)
            checks = selected(result, 'host_checks') | selected(result, 'game_checks')
            if not checks and not result['boot'][0]:
                failures.append(f'{source} selects nothing (builds: {builds.name})')
            boot_only += builds is fake and not checks
            if builds is fake and not checks and not result['boot'][1].endswith('owned by no check') \
                    and 'boot build reads' not in result['boot'][1] and 'run time' not in result['boot'][1]:
                failures.append(f'{source}: no check and not in the boot graph, but {result["boot"]}')
    print(f'{len(sources)} source files; {boot_only} select no host or game-data check, only the boot stage')
    expect(boot_only > 0, 'no source file was owned by no check: the orphan rule went untested')

if failures:
    print('\n'.join(failures))
    sys.exit(f'{len(failures)} affected.py failures')
print('affected.py checks passed')
