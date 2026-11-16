#!/usr/bin/env python3
"""Exercise the production generator worker with deterministic inference and thermal inputs, no GPU/models."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
check=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="storm-relief-scheduler-") as directory:
    output=Path(directory)/"check"
    source=Path(os.environ.get('STORM_RELIEF_GENERATOR_SOURCE',str(root/'src/libs/renderer_bgfx/src/effects/relief_generator.cpp')))
    worker=Path(directory)/'worker.cpp'
    text=source.read_text()
    # Shorten only the polling cadence; the thermal predicate and all queue policy stay the production ones.
    poll='constexpr auto kWaitPause = std::chrono::seconds(5);'
    assert text.count(poll)==1, 'worker polling cadence changed'
    text=text.replace(poll,'constexpr auto kWaitPause = std::chrono::milliseconds(10);')
    assert text.count('using Clock = std::chrono::steady_clock;')==1, 'worker clock declaration changed'
    text=text.replace('using Clock = std::chrono::steady_clock;',
        'struct Clock : std::chrono::steady_clock { static time_point now() { return test_now(); } };')
    worker.write_text('#include <chrono>\nstd::chrono::steady_clock::time_point test_now();\n'+text)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror',
        '-fno-access-control', # fixture backdates the quiet deadline under the real queue mutex; no production hook
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(check/'stubs'),'-I'+str(root/'src/libs/renderer_bgfx/src'),
        '-I'+str(root/'src/libs/renderer_bgfx/include'),'-I'+str(root/'src/libs/renderer_bgfx/fx/include'),
        '-I'+str(root/'src/libs/relief'),str(check/'check.cpp'),
        str(worker),
        str(root/'src/libs/renderer_bgfx/fx/src/upscale.cpp'),'-o',str(output)],check=True)
    # The check waits on progress only; DEADLOCK_GUARD is not a time budget but the point at which a step that
    # cannot finish is reported (with the last step it printed). An idle run takes about 2 s.
    DEADLOCK_GUARD=300
    try:
        result=subprocess.run([str(output),directory],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=DEADLOCK_GUARD)
    except subprocess.TimeoutExpired as hung:
        steps=(hung.stdout or b'').decode(errors='replace').strip().splitlines()
        sys.exit(f'FAIL: no progress for {DEADLOCK_GUARD} s (a worker deadlock) at {steps[-1] if steps else "start"}')
    print(result.stdout,end='')
    sys.exit(result.returncode)
