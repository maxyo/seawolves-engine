#!/usr/bin/env python3
"""Land commits on engine develop through one local merge queue, gated in batches.

  tools/ci/land.py [REF] [--tests NAME ...]     queue REF (default HEAD) and wait until it landed or failed
  tools/ci/land.py --status                     what is queued, running and done
  STORM_LAND_DRY=1 tools/ci/land.py ...         everything but the push (the runner's batch is not pushed)

Every worktree of this repository shares the queue (<git common dir>/storm-land). Whoever finds no
runner becomes the runner; the others wait for their result. The runner works in its own worktree,
<workspace>/storm-engine-land (warm build directories, created on first use), and repeats:

1. take every queued entry; rebase each one's commits (merge-base..REF, merges dropped like
   `git rebase`) onto origin/develop and the entries before it. An entry that conflicts fails
   ("rebase conflicts") and is left out;
2. gate the stacked tree once (tools/ci/gate.py: what the union of the changes affects, plus every
   entry's --tests), with --boot auto: one boot smoke for runtime-affecting changes in the batch;
3. passed: push to develop when origin/develop has not moved (if it moved, start again: the builds are
   incremental), and report the landed commit to each entry;
   failed with several entries: gate them one by one on top of each other, keep the ones that pass,
   push those, and report each failing entry with its gate logs. Boot is selected only from the current
   entry's changes, so a passing entry does not cause another entry's boot run.

The gate's environment is the runner's, updated by <git common dir>/storm-land/env (NAME=VALUE lines,
# comments): the queue's own settings, the same whichever agent becomes the runner. The boot stage needs
STORM_VIRTUALGL (the VirtualGL directory, tools/scene_sweep/README.md); without it land.py refuses to queue.

A gate failure, conflict or refused push is reported to its entry with the logs
(<git common dir>/storm-land/logs/<id>/); nothing is dropped silently. After landing, the agent's own
branch still holds the unrebased commits: `git fetch && git rebase origin/develop` makes it equal.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[2]
common = Path(subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                      text=True).strip())
state = common / 'storm-land'
queue, done_dir, log_root = state / 'queue', state / 'done', state / 'logs'
env_file = state / 'env'
land_tree = Path(os.environ.get('STORM_LAND_WORKTREE', common.parent.parent / 'storm-engine-land'))
BASE = 'origin/develop'


def git(*args, cwd=None, check=True):
    result = subprocess.run(['git', '-C', str(cwd or land_tree), *args], capture_output=True, text=True)
    if check and result.returncode:
        raise RuntimeError(f'git {" ".join(args)}: {result.stderr.strip()}')
    return result


def gate_env():
    """The runner's environment updated by the queue's env file; RuntimeError when the boot stage could not run."""
    settings = {}
    if env_file.exists():
        for number, line in enumerate(env_file.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            name, equals, value = line.partition('=')
            if not equals or not name.strip():
                raise RuntimeError(f'{env_file}:{number}: expected NAME=VALUE, got {line!r}')
            settings[name.strip()] = value.strip()
    env = dict(os.environ, **settings)
    vgl = env.get('STORM_VIRTUALGL')
    if not vgl or not (Path(vgl) / 'usr/lib/libvglfaker.so').is_file():
        raise RuntimeError(f'the queue gates with --boot auto, which renders through VirtualGL: STORM_VIRTUALGL is '
                 f'{vgl or "not set"}, not a VirtualGL directory (usr/lib/libvglfaker.so). Set it in {env_file} '
                 f'as STORM_VIRTUALGL=<dir> (tools/scene_sweep/README.md, Renderer)')
    return env


def say(message):
    line = time.strftime('%H:%M:%S ') + message
    print(line, flush=True)
    with open(state / 'runner.log', 'a') as log:
        log.write(line + '\n')


def finish(entry, status, detail, **extra):
    record = dict(entry, status=status, detail=detail, finished=time.time(), **extra)
    (done_dir / f'{entry["id"]}.json').write_text(json.dumps(record, indent=1))
    (queue / f'{entry["id"]}.json').unlink(missing_ok=True)
    say(f'{entry["label"]} {entry["sha"][:8]}: {status}: {detail}')


def gate(entries, tag, boot_base=None):
    """Runs the gate in the land worktree; returns (passed, output) and keeps the logs."""
    tests = sorted({t for e in entries for t in e.get('tests', [])})
    command = [sys.executable, str(land_tree / 'tools/ci/gate.py'), '--no-fetch', '--base', BASE, '--boot', 'auto']
    if boot_base is not None:
        command += ['--boot-base', boot_base]
    if tests:
        command += ['--tests', *tests]
    say(f'gate {tag}: ' + ', '.join(f'{e["label"]} {e["sha"][:8]}' for e in entries))
    result = subprocess.run(command, cwd=land_tree, env=gate_env(), capture_output=True, text=True)
    output = result.stdout + result.stderr
    for e in entries:
        target = log_root / e['id'] / tag
        target.mkdir(parents=True, exist_ok=True)
        (target / 'gate.txt').write_text(output)
        for log in (land_tree / 'build/gate').glob('*'):
            if log.is_dir():
                # Boot reports/pictures are evidence; sweep work holds disposable copies of the whole game.
                shutil.copytree(log, target / log.name, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns('work'))
            else:
                shutil.copy(log, target / log.name)
    return result.returncode == 0, output


def apply(entry):
    """Rebases the entry's commits onto the current stack; False on conflicts (stack unchanged)."""
    tip = git('rev-parse', 'HEAD').stdout.strip()
    base = git('merge-base', BASE, entry['sha']).stdout.strip()
    result = git('rebase', '--onto', tip, base, entry['sha'], check=False)
    if result.returncode:
        git('rebase', '--abort', check=False)
        git('checkout', '-q', '--detach', tip)
        return False, (result.stdout + result.stderr).strip()[-2000:]
    return True, ''


def prepare_tree():
    if not (land_tree / '.git').exists():
        say(f'creating the land worktree {land_tree}')
        git('worktree', 'add', '--detach', str(land_tree), BASE, cwd=root)
    git('fetch', '-q', 'origin')
    git('rebase', '--abort', check=False)
    git('checkout', '-q', '-f', '--detach', BASE)
    return git('rev-parse', 'HEAD').stdout.strip()


def push(entries, base_sha):
    git('fetch', '-q', 'origin')
    if git('rev-parse', BASE).stdout.strip() != base_sha:
        return 'moved'
    head = git('rev-parse', 'HEAD').stdout.strip()
    if os.environ.get('STORM_LAND_DRY'):
        for e in entries:
            finish(e, 'dry run', f'gate passed; would push {head[:8]} to develop (STORM_LAND_DRY)', tree=head)
        return 'pushed'
    result = git('push', 'origin', 'HEAD:develop', check=False)
    if result.returncode:
        for e in entries:
            finish(e, 'push refused', f'gated tree {head} could not be pushed: {result.stderr.strip()[-500:]}', tree=head)
        return 'refused'
    for e in entries:
        finish(e, 'landed', f'develop is {head[:8]}', develop=head)
    return 'pushed'


def run_batch(batch):
    base_sha = prepare_tree()
    stacked = []
    for entry in batch:
        ok, why = apply(entry)
        if ok:
            stacked.append(entry)
        else:
            finish(entry, 'failed', f'rebase onto develop {base_sha[:8]} and the entries before it conflicts: {why}')
    if not stacked:
        return
    passed, output = gate(stacked, 'batch')
    if passed:
        if push(stacked, base_sha) == 'moved':
            say('origin/develop moved during the gate: again on the new develop')
        return
    if len(stacked) == 1:
        finish(stacked[0], 'failed', 'gate failed: ' + output.strip().splitlines()[-1],
               logs=str(log_root / stacked[0]['id'] / 'batch'))
        return
    say(f'batch of {len(stacked)} failed: gating the entries one by one')
    base_sha = prepare_tree()
    kept = []
    last_gate_ok = True
    for entry in stacked:
        tip = git('rev-parse', 'HEAD').stdout.strip()
        ok, why = apply(entry)
        if not ok:
            finish(entry, 'failed', f'rebase conflicts: {why}')
            continue
        passed, output = gate(kept + [entry], f'single-{entry["id"]}', boot_base=tip)
        if passed:
            kept.append(entry)
            last_gate_ok = True
        else:
            finish(entry, 'failed', 'gate failed: ' + output.strip().splitlines()[-1],
                   logs=str(log_root / entry['id'] / f'single-{entry["id"]}'))
            git('checkout', '-q', '-f', '--detach', tip)
            last_gate_ok = bool(kept)  # the previous passing gate covered exactly the kept stack
    if kept and last_gate_ok and push(kept, base_sha) == 'moved':
        say('origin/develop moved during the gate: again on the new develop')


def entries():
    return sorted((json.loads(p.read_text()) for p in queue.glob('*.json')), key=lambda e: e['queued'])


def runner():
    """Runs batches while the queue is not empty; returns when it is."""
    while True:
        batch = entries()
        if not batch:
            return
        try:
            run_batch(batch)
        except Exception as error:  # a broken runner must not leave entries waiting forever
            for entry in batch:
                if (queue / f'{entry["id"]}.json').exists():
                    finish(entry, 'failed', f'land runner error: {error}')


def status():
    for e in entries():
        print(f'queued  {e["label"]} {e["sha"][:8]} since {time.strftime("%H:%M", time.localtime(e["queued"]))}')
    for p in sorted(done_dir.glob('*.json'), key=lambda p: p.stat().st_mtime)[-10:]:
        e = json.loads(p.read_text())
        print(f'{e["status"]:8} {e["label"]} {e["sha"][:8]}: {e["detail"][:150]}')
    print(f'gate env: {env_file}' + ('' if env_file.exists() else ' (missing)'))
    if (state / 'runner.log').exists():
        print('runner log: ' + str(state / 'runner.log'))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('ref', nargs='?', default='HEAD')
    parser.add_argument('--tests', nargs='*', default=[])
    parser.add_argument('--status', action='store_true')
    args = parser.parse_args()
    for d in (queue, done_dir, log_root):
        d.mkdir(parents=True, exist_ok=True)
    if args.status:
        status()
        return
    try:
        gate_env()  # every caller can become the runner: refuse to queue rather than fail every runtime batch later
    except RuntimeError as error:
        sys.exit(f'land: {error}')
    if git('status', '--porcelain', '--untracked-files=no', cwd=Path.cwd()).stdout.strip() and args.ref == 'HEAD':
        print('land: note, uncommitted changes are not landed (only the commits of HEAD)', flush=True)
    sha = git('rev-parse', '--verify', args.ref + '^{commit}', cwd=Path.cwd()).stdout.strip()
    label = git('rev-parse', '--abbrev-ref', args.ref, cwd=Path.cwd()).stdout.strip()
    if label in ('HEAD', ''):
        label = Path.cwd().name
    entry = {'id': f'{int(time.time() * 1000)}-{sha[:8]}', 'sha': sha, 'label': label, 'tests': args.tests,
             'queued': time.time(), 'from': str(Path.cwd())}
    (queue / f'{entry["id"]}.json').write_text(json.dumps(entry, indent=1))
    print(f'land: queued {label} {sha[:8]} ({entry["id"]})', flush=True)
    result = done_dir / f'{entry["id"]}.json'
    lock = open(state / 'lock', 'w')
    while not result.exists():
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            time.sleep(5)  # another agent's runner has the queue; it takes this entry in its next batch
            continue
        try:
            say(f'runner: pid {os.getpid()} from {Path.cwd()}')
            runner()
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    record = json.loads(result.read_text())
    print(f'land: {record["status"]}: {record["detail"]}', flush=True)
    if record.get('logs'):
        print(f'land: logs in {record["logs"]}', flush=True)
    sys.exit(0 if record['status'] in ('landed', 'dry run') else 1)


if __name__ == '__main__':
    main()
