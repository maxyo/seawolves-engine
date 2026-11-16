#!/usr/bin/env python3
"""Run every host-side engine check (no NDK, SDK, device or game data).

Fetches the pinned open-source sources the checks compile against (the same
revisions as tools/android/ci_dependencies.sh, tools/android/engine_sources.py
and cmake/glm.cmake), then runs each tests/*/run.py with the arguments it
declares (sources, glm, a build directory). Every failure is reported; the exit code is non-zero if
any check failed. CI runs it on pushes and pull requests to develop.

  python3 tools/ci/run_host_tests.py [--work build/host-ci] [--jobs N] [--only NAME ...] [--changed-since REF]

--changed-since REF runs only the checks the changes since REF can affect (tools/ci/affected.py) and
prints every check it leaves out with the reason. Checks start longest first (the times of the last
run, <work>/durations.json), so the slowest one does not start last.

The SDL input check needs a display: run under xvfb-run when headless.
"""
import argparse
import concurrent.futures
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time
import urllib.request

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--work', type=Path, default=root / 'build/host-ci')
parser.add_argument('--only', nargs='*', help='run only these test directory names')
parser.add_argument('--jobs', type=int, default=os.cpu_count(), help='checks run in parallel')
parser.add_argument('--changed-since', metavar='REF', help='only the checks the changes since REF affect')
args = parser.parse_args()
work = args.work.resolve()
sources = work / 'sources'


def checkout(name, url, revision):
    target = sources / name
    if not (target / '.git').exists():
        subprocess.run(['git', 'init', '-q', str(target)], check=True)
        subprocess.run(['git', '-C', str(target), 'remote', 'add', 'origin', url], check=True)
    head = subprocess.run(['git', '-C', str(target), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if head != revision:
        subprocess.run(['git', '-C', str(target), 'fetch', '-q', '--depth', '1', 'origin', revision], check=True)
        subprocess.run(['git', '-C', str(target), 'checkout', '-q', '--detach', 'FETCH_HEAD'], check=True)


def prepare_sources():
    sources.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(root / 'tools/android/engine_sources.py'), '--directory', str(sources)],
                   check=True, stdout=subprocess.DEVNULL)
    pipeline = (root / 'tools/android/ci_dependencies.sh').read_text()
    match = re.search(r'^checkout_source SDL (\S+) ([0-9a-f]{40})$', pipeline, re.M)
    if not match:
        sys.exit('SDL revision not found in tools/android/ci_dependencies.sh')
    checkout('SDL', match[1], match[2])
    glm = work / 'glm'
    url = re.search(r'URL (https://\S+)', (root / 'cmake/glm.cmake').read_text())[1]
    stamp = glm / '.storm-url'
    if not stamp.exists() or stamp.read_text() != url:
        with urllib.request.urlopen(url) as response:
            archive = tarfile.open(fileobj=io.BytesIO(response.read()))
        glm.mkdir(parents=True, exist_ok=True)
        archive.extractall(glm, filter='data')
        stamp.write_text(url)
    return next(path for path in glm.iterdir() if path.is_dir())


glm_source = prepare_sources()
os.environ.setdefault('STORM_GLM_SOURCE', str(glm_source))  # checks that locate GLM themselves
values = {
    '--sdl-source': sources / 'SDL',
    '--fmt-source': sources / 'fmt',
    '--dependency-sources': sources,
    '--glm-source': glm_source,
}
checks = sorted(root.glob('tests/*/run.py'))
if args.only is not None:
    unknown = sorted(set(args.only) - {path.parent.name for path in checks})
    if unknown:
        sys.exit('--only names no such check (tests/<name>/run.py): ' + ', '.join(unknown))
    checks = [path for path in checks if path.parent.name in args.only]
if args.changed_since:
    sys.path.insert(0, str(root / 'tools/ci'))
    import affected
    plan = affected.host_checks(affected.changed_files(args.changed_since), work)
    for name, (selected, why) in plan.items():
        if not selected and any(path.parent.name == name for path in checks):
            print(f'SKIP {name}: {why}', flush=True)
    checks = [path for path in checks if plan[path.parent.name][0]]
durations_file = work / 'durations.json'
try:
    durations = json.loads(durations_file.read_text())
except (OSError, ValueError):
    durations = {}
# Longest first; unknown ones (new checks) first of all.
checks.sort(key=lambda path: -durations.get(path.parent.name, 1e9))
# Options a check declares, with either quote style.
OPTION = r'''add_argument\(\s*['"](--[a-z-]+)['"]'''


def command_for(script):
    """The check's command line, or the reason the host cannot run it (a FAIL of that check)."""
    name = script.parent.name
    command = [sys.executable, str(script)]
    text = script.read_text()
    for option in re.findall(OPTION, text):
        if option == '--build-dir':
            command += [option, str(work / 'build' / name)]
        elif option in values:
            command += [option, str(values[option])]
    missing = [option for option in re.findall(OPTION + r'[^)]*required\s*=\s*True', text) if option not in command]
    if missing:
        return None, f'{script.relative_to(root)}: no host value for required option {", ".join(missing)}'
    return command, None


# Checks that run the compiler themselves compile through the Storm ccache launcher
# (tools/ccache/cxx); CMake-based checks get it from cmake/StormBuildCache.cmake.
direct_environment = dict(os.environ, CXX=str(root / 'tools/ccache/cxx'),
                          STORM_CCACHE_CXX=os.environ.get('CXX', 'c++'))


deps_dir = work / 'deps'


def run(script):
    """(name, exit code, output, seconds). Never raises: whatever stops a check is that check's FAIL.

    A check that runs the compiler itself records what its compiles read (tools/ccache/cxx STORM_CXX_DEPS) in
    <work>/deps/<name>.txt when it passes; tools/ci/affected.py selects it by that list. A failed run leaves none."""
    name = script.parent.name
    started = time.monotonic()
    deps = deps_dir / f'{name}.txt'
    partial = deps_dir / f'{name}.txt.partial'
    try:
        deps.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)
        command, problem = command_for(script)
        if problem:
            returncode, output = 1, problem + '\n'
        else:
            environment = None
            if not (script.parent / 'CMakeLists.txt').exists():
                environment = dict(direct_environment, STORM_CXX_DEPS=str(partial))
            result = subprocess.run(command, cwd=root, env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            returncode, output = result.returncode, result.stdout
        if returncode == 0 and partial.exists():
            deps.write_text(''.join(f'{line}\n' for line in sorted(set(partial.read_text().splitlines()))))
        partial.unlink(missing_ok=True)
    except Exception as error:  # reported as this check's FAIL; the other checks keep running
        returncode, output = 1, f'{script.relative_to(root)}: the runner could not run it: {error!r}\n'
    return name, returncode, output, time.monotonic() - started


deps_dir.mkdir(parents=True, exist_ok=True)
failed = []
with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
    for future in concurrent.futures.as_completed([pool.submit(run, check) for check in checks]):
        name, returncode, output, elapsed = future.result()
        durations[name] = round(elapsed, 1)
        print(f'{"PASS" if returncode == 0 else "FAIL"} {name} ({elapsed:.1f}s)', flush=True)
        if returncode:
            print(output[-20000:], flush=True)
            failed.append(name)
work.mkdir(parents=True, exist_ok=True)
durations_file.write_text(json.dumps(dict(sorted(durations.items())), indent=0))
if failed:
    sys.exit(f'{len(failed)} of {len(checks)} host checks failed: ' + ', '.join(failed))
print(f'All {len(checks)} host checks passed')
