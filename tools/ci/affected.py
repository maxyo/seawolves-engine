#!/usr/bin/env python3
"""What a change can affect: the host checks (tests/*/run.py), the game-data checks (tools/game_checks),
the builds a gate runs (the UI host build, the Android engine build) and the engine boot stage
(tools/ci/boot_stage.py). Used by tools/ci/gate.py and `run_host_tests.py --changed-since`; every check or
build it leaves out is reported with the reason.

  affected.py [--base origin/develop] [--head HEAD] [--json]

Rules (README.md here):
- Markdown files (*.md) are documentation: they affect nothing.
- A change to the host CI itself (.github, tools/ci, tools/ccache, cmake, the top-level CMakeLists.txt,
  tests/<file>) selects everything.
- A host check that is a CMake project and has been built in --work is decided by its build graph: it
  is affected when a changed file is an input of that build (Ninja's dependency log and build.ninja),
  a new file lies in a directory the build reads, or its own directory changed.
- Any other host check is affected when its own directory changed, or when its files name a changed
  file, the changed file's directory, or the changed file's component (src/libs/<name>, src/apps/<name>,
  <top>/<name>), with or without the final slash (`src/libs/relief"`, `root/'src/libs/relief'`), but
  never as the start of a longer name (src/libs/relief is not src/libs/relief_ui).
- A build (UI host, Android engine) is affected the same way, from its build directory's graph; without
  a built directory it is affected ("no build graph yet").
- A game-data check is affected when its own directory changed, when its built graph (build/game-checks/
  <name>) reads a changed file or, not built yet, its files name one as above, or when a changed file is
  one of the headers its check.cpp includes (the compiler's -MM list).
- The engine boot stage (a sanitizer host engine boots, loads a save and three scenes) is affected by every
  change that can change what the engine does at run time: an input of its build graph (build/boot-asan;
  not built yet: any file under src/ or android/), the run-time renderer input src/techniques, and the
  sweep and boot tools themselves. A changed source file no host or game-data check selects selects the
  boot stage ("owned by no check"), never nothing.
"""
import argparse
import functools
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
EVERYTHING = ('.github/', 'tools/ci/', 'tools/ccache/', 'cmake/')
TEXT_SUFFIXES = ('.py', '.txt', '.cpp', '.hpp', '.h', '.cmake', '.c', '.json', '.ini', '.sh')
# Source files: what the engine builds compile or the engine reads at run time.
SOURCE_ROOTS = ('src/', 'android/')
SOURCE_SUFFIXES = ('.c', '.cpp', '.h', '.hpp', '.inc', '.inl', '.sc', '.sh', '.fx', '.json', '.txt', '.cmake',
                   '.patch', '.java', '.def', '.rml', '.rcss')
# Read by the engine at run time and by no build: the scene sweep lays the techniques over the game's, as the APK.
RUNTIME_INPUTS = ('src/techniques/',)
BOOT_TOOLS = ('tools/scene_sweep/', 'tools/ci/boot_stage.py')
# Directories of libraries, not one component: their component is one level deeper.
CONTAINERS = ('src/libs/', 'src/apps/')
PATH_TOKEN = re.compile(r'[\w./-]+')


def changed_files(base, head='HEAD', repo=root):
    """Files changed between the merge base of base and head, and head (plus uncommitted ones when head is HEAD)."""
    out = subprocess.check_output(['git', '-C', str(repo), 'diff', '--name-status', '--no-renames', f'{base}...{head}'],
                                  text=True).splitlines()
    if head == 'HEAD':
        out += subprocess.check_output(['git', '-C', str(repo), 'diff', '--name-status', '--no-renames', 'HEAD'],
                                       text=True).splitlines()
    files = {}
    for line in out:
        status, path = line.split('\t', 1)
        if path.endswith('.md'):
            continue  # documentation: read by no build or check
        files[path] = 'A' if status.startswith('A') and files.get(path, 'A') == 'A' else 'M'
    return files


def is_source(path):
    return path.startswith(SOURCE_ROOTS) and path.endswith(SOURCE_SUFFIXES)


def component(path):
    """src/libs/<name>, src/apps/<name>; otherwise <top>/<name> (src/techniques, tools/scene_sweep). None at the top."""
    parts = path.split('/')
    depth = 3 if path.startswith(CONTAINERS) else 2
    return '/'.join(parts[:depth]) if len(parts) > depth else None


def keys_of(path):
    """What a check may spell to name a changed file: the file, its directory, its component."""
    return [key for key in dict.fromkeys((path, os.path.dirname(path), component(path))) if key]


@functools.cache
def fragments(directory):
    """Every path a check's text files spell, and each of its parts that starts and ends at a '/' or at the
    spelling's ends: '../../src/libs/relief"' gives src/libs/relief, libs/relief, src/libs, ... but never a
    prefix of a longer name."""
    found = set()
    for file in sorted(Path(directory).rglob('*')):
        if not file.is_file() or file.suffix not in TEXT_SUFFIXES:
            continue
        for token in PATH_TOKEN.findall(file.read_text(errors='ignore')):
            if '/' not in token:
                continue
            parts = token.split('/')
            for start in range(len(parts)):
                for end in range(start + 1, len(parts) + 1):
                    found.add('/'.join(parts[start:end]))
    return frozenset(found)


def names(directory, files):
    """(key, changed file) for the first changed file the check's files name, or None."""
    spelled = fragments(str(directory))
    for path in files:
        for key in keys_of(path):
            if key in spelled:
                return key, path
    return None


class BuildGraph:
    """Every source path a Ninja build directory reads: the dependency log, the explicit inputs and the
    configure inputs of build.ninja. None when the directory has never been built."""

    def __init__(self, build_dir):
        self.build_dir = Path(build_dir)
        self.inputs = None
        ninja = shutil.which('ninja') or str(root.parent / 'storm-engine-android/build/android-ci/python/bin/ninja')
        if not (self.build_dir / '.ninja_deps').exists() or not (self.build_dir / 'build.ninja').exists():
            return
        paths = set()
        deps = subprocess.run([ninja, '-C', str(self.build_dir), '-t', 'deps'], capture_output=True, text=True)
        for line in deps.stdout.splitlines():
            if line.startswith('    '):
                paths.add(line.strip())
        inputs = subprocess.run([ninja, '-C', str(self.build_dir), '-t', 'inputs'], capture_output=True, text=True)
        if inputs.returncode != 0:  # Ninja older than 1.11: no 'inputs' tool, the dependency log alone is not enough
            return
        paths.update(line.strip() for line in inputs.stdout.splitlines())
        # The configure inputs (CMakeLists.txt, *.cmake, patches): the implicit inputs of CMake's re-run edge.
        for line in (self.build_dir / 'build.ninja').read_text(errors='ignore').splitlines():
            if line.startswith('build build.ninja') and ': RERUN_CMAKE' in line and ' | ' in line:
                paths.update(p.replace('$ ', ' ') for p in line.split(' | ', 1)[1].split(' '))
        resolved = set()
        for p in paths:
            path = Path(p) if os.path.isabs(p) else self.build_dir / p
            try:
                resolved.add(os.path.realpath(path))
            except OSError:
                pass
        self.inputs = resolved
        self.dirs = {os.path.dirname(p) for p in resolved}

    def hit(self, files, repo=root):
        """The first changed file the build reads, or None."""
        for path, status in files.items():
            full = os.path.realpath(repo / path)
            if full in self.inputs:
                return path
            if status == 'A' and os.path.dirname(full) in self.dirs:
                return f'{path} (new, in a directory the build reads)'
        return None


@functools.cache
def graph(build_dir):
    """The build graph of a directory, read once per process."""
    return BuildGraph(build_dir)


def everything_reason(files):
    for path in files:
        if path.startswith(EVERYTHING) or path == 'CMakeLists.txt' or (path.startswith('tests/') and path.count('/') == 1):
            return f'{path} changes the host CI itself'
    return None


@functools.cache
def check_directories(directory):
    """((name, is a CMake project), ...) of the checks under a directory (tests, tools/game_checks)."""
    return tuple((p.parent.name, (p.parent / 'CMakeLists.txt').exists()) for p in sorted(Path(directory).glob('*/run.py')))


def host_checks(files, work=None, repo=root):
    """{name: (affected, reason)} for every tests/<name>/run.py."""
    every = everything_reason(files)
    result = {}
    for name, cmake in check_directories(str(repo / 'tests')):
        own = next((p for p in files if p.startswith(f'tests/{name}/')), None)
        if every:
            result[name] = (True, every)
            continue
        if own:
            result[name] = (True, f'its own {own} changed')
            continue
        # A built CMake check: its build graph knows what it reads, whatever its files spell.
        if work and cmake:
            built = graph(str(Path(work) / 'build' / name))
            if built.inputs is not None:
                hit = built.hit(files, repo)
                result[name] = (True, f'its build reads {hit}') if hit else (
                    False, f'no changed file is an input of its build (ninja graph of build/{name})')
                continue
        found = names(repo / 'tests' / name, files)
        result[name] = (True, f'names {found[0]} ({found[1]})') if found else (
            False, 'names no changed file, directory or component')
    return result


def build_affected(build_dir, files, label, repo=root):
    every = everything_reason(files)
    if every:
        return True, every
    if any(p.startswith('tools/android/') for p in files):
        return True, 'tools/android changed'
    built = graph(str(build_dir))
    if built.inputs is None:
        return True, f'{label}: no build graph yet in {build_dir}'
    hit = built.hit(files, repo)
    return (True, f'it reads {hit}') if hit else (False, f'no changed file is an input of {label} ({build_dir})')


@functools.cache
def included_headers(check, repo, cxx):
    """The headers a game check's check.cpp includes (the -I directories its run.py passes, compiler -MM), or the
    compiler's error text."""
    check, repo = Path(check), Path(repo)
    includes = ['-I' + str(repo / m) for m in re.findall(r"'-I' \+ str\(root / '([^']+)'\)", check.read_text())]
    dep = subprocess.run([cxx, '-std=c++20', '-MM', *includes, str(check.parent / 'check.cpp')],
                         capture_output=True, text=True)
    if dep.returncode != 0:
        return dep.stderr.strip()[:200]
    return frozenset(os.path.realpath(h) for h in dep.stdout.replace('\\\n', ' ').split()[1:])


def game_checks(files, repo=root, builds=None):
    """{name: (affected, reason)} for every tools/game_checks/<name>/run.py; builds: their build directories'
    parent (default <repo>/build/game-checks)."""
    builds = Path(builds or repo / 'build/game-checks')
    result = {}
    every = everything_reason(files)
    cxx = os.environ.get('STORM_CCACHE_CXX', 'c++')
    for check in sorted((repo / 'tools/game_checks').glob('*/run.py')):
        name = check.parent.name
        own = next((p for p in files if p.startswith(f'tools/game_checks/{name}/')), None)
        if every or own:
            result[name] = (True, every or f'its own {own} changed')
            continue
        # The libraries it links (its CMakeLists.txt): its build graph once built, else what its files name.
        if (check.parent / 'CMakeLists.txt').exists():
            built = graph(str(builds / name))
            hit = built.hit(files, repo) if built.inputs is not None else None
            found = names(check.parent, files) if built.inputs is None else None
            if hit or found:
                result[name] = (True, f'its build reads {hit}' if hit else f'names {found[0]} ({found[1]})')
                continue
        headers = included_headers(str(check), str(repo), cxx)
        if isinstance(headers, str):
            result[name] = (True, 'its include list could not be computed: ' + headers)
            continue
        hit = next((p for p in files if os.path.realpath(repo / p) in headers), None)
        result[name] = (True, f'check.cpp includes {hit}') if hit else (False, 'check.cpp includes no changed file')
    return result


def boot_stage(files, build_dir, orphans=(), repo=root):
    """(affected, reason) of the engine boot stage (tools/ci/boot_stage.py). orphans: the changed source files no
    host or game-data check selects."""
    every = everything_reason(files)
    if every:
        return True, every
    tool = next((p for p in files if p.startswith(BOOT_TOOLS)), None)
    if tool:
        return True, f'{tool} changed (the boot stage itself)'
    data = next((p for p in files if p.startswith(RUNTIME_INPUTS)), None)
    if data:
        return True, f'{data} is read by the engine at run time'
    built = graph(str(build_dir))
    if built.inputs is None:
        engine = next((p for p in files if p.startswith(SOURCE_ROOTS)), None)
        if engine:
            return True, f'{engine} (the boot build {build_dir} has no build graph yet)'
    else:
        hit = built.hit(files, repo)
        if hit:
            return True, f'the boot build reads {hit}'
    if orphans:
        return True, f'{orphans[0]} is owned by no check'
    return False, 'no changed file can change what the engine does at run time'


def plan(files, work=None, repo=root, builds=None):
    """Every check and stage of a change: {group: {name: (affected, reason)}} for host_checks and game_checks,
    (affected, reason) for ui_host, android and boot. builds: the build directories' parent (default <repo>/build)."""
    builds = Path(builds or repo / 'build')
    work = work if work is not None else builds / 'host-ci'
    host = host_checks(files, work, repo)
    games = game_checks(files, repo, builds / 'game-checks')
    orphans = []
    for path in (p for p in files if is_source(p)):
        alone = {path: files[path]}
        if not any(run for run, _ in host_checks(alone, work, repo).values()) and \
                not any(run for run, _ in game_checks(alone, repo, builds / 'game-checks').values()):
            orphans.append(path)
    return {
        'host_checks': host,
        'game_checks': games,
        'ui_host': build_affected(builds / 'ui-host', files, 'the UI host build', repo),
        'android': build_affected(builds / 'android-engine', files, 'the Android engine build', repo),
        'boot': boot_stage(files, builds / 'boot-asan', orphans, repo),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', default='origin/develop')
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--work', type=Path, default=root / 'build/host-ci')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    files = changed_files(args.base, args.head)
    result = {'changed': files, **plan(files, args.work)}
    if args.json:
        json.dump(result, sys.stdout, indent=1)
        return
    print(f'{len(files)} changed files')
    for group in ('host_checks', 'game_checks'):
        for name, (run, why) in result[group].items():
            print(f'{"RUN " if run else "skip"} {group[:-1]} {name}: {why}')
    for stage in ('ui_host', 'android', 'boot'):
        run, why = result[stage]
        print(f'{"RUN " if run else "skip"} {stage}: {why}')


if __name__ == '__main__':
    main()
