#!/usr/bin/env python3
"""Check of tools/ci/check_tmp.py, the TMPDIR every gate check runs with: what a killed check leaves goes into the
runner's directory instead of /tmp, the next run starts empty, a pass removes the directory, a failure keeps it."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/ci'))
sys.dont_write_bytecode = True
import check_tmp  # noqa: E402

failures = []


def expect(condition, text):
    if not condition:
        failures.append(text)


# A child that makes a temporary directory the usual way and dies before it can remove it.
KILLED = ("import os, signal, tempfile; d = tempfile.mkdtemp(prefix='storm-killed-'); print(d, flush=True); "
          "os.kill(os.getpid(), signal.SIGKILL)")

with tempfile.TemporaryDirectory(prefix='storm-check-tmp-') as base:
    tmp = Path(base) / 'build/host-ci/tmp/some_check'
    env = check_tmp.prepare(tmp)
    expect(env['TMPDIR'] == str(tmp) and tmp.is_dir() and not any(tmp.iterdir()), 'prepare: an empty TMPDIR')
    child = subprocess.run([sys.executable, '-c', KILLED], env=env, capture_output=True, text=True)
    left = Path(child.stdout.strip())
    expect(child.returncode != 0 and left.parent == tmp and left.is_dir(),
           f'a killed check leaves its directory in TMPDIR, not /tmp: {left}')

    check_tmp.finish(tmp, passed=False)
    expect(left.is_dir(), 'a failed check keeps its files for diagnosis')
    check_tmp.prepare(tmp)
    expect(tmp.is_dir() and not any(tmp.iterdir()), 'the next run starts with an empty TMPDIR')
    (tmp / 'file').write_text('x')
    check_tmp.finish(tmp, passed=True)
    expect(not tmp.exists(), 'a pass removes the TMPDIR')

for failure in failures:
    print('FAIL:', failure)
print('FAIL: ci_check_tmp' if failures else 'PASS: ci_check_tmp')
sys.exit(1 if failures else 0)
