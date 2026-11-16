#!/usr/bin/env python3
"""Real generator + FinalizeQueue, inert compute/storage. No native models/GPU.
--baseline-revision compiles the old generator with matching API fixture and requires runtime non-overlap.
The check waits on progress only; DEADLOCK_GUARD is not a time budget but the point at which a step that cannot
finish is reported (with the last step it printed).
"""
from pathlib import Path
import argparse,os,subprocess,sys,tempfile
DEADLOCK_GUARD=300 # seconds; an idle run takes well under 1 s
p=argparse.ArgumentParser();p.add_argument('--baseline-revision');a=p.parse_args()
root=Path(__file__).resolve().parents[2];here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='relief-staged-',dir=os.environ.get('TMPDIR')) as d:
 d=Path(d);binary=d/'check'
 worker=root/'src/libs/renderer_bgfx/src/effects/relief_generator.cpp'
 if a.baseline_revision:
  worker=d/'old-generator.cpp';worker.write_bytes(b'#include "finalize_queue.hpp"\n'+subprocess.check_output(['git','show',a.baseline_revision+':src/libs/renderer_bgfx/src/effects/relief_generator.cpp']))
 command=[os.environ.get('CXX','c++'),'-std=c++20','-O0','-g','-pthread','-Wall','-Wextra','-Werror','-fno-access-control','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']
 if a.baseline_revision:command+=['-DSTORM_STAGED_BASELINE=1']
 for path in ['tests/relief_scheduler/stubs','src/libs/renderer_bgfx/src','src/libs/renderer_bgfx/include','src/libs/renderer_bgfx/fx/include','src/libs/relief','src/libs/renderer_bgfx/src/texture/core']:command+=['-I'+str(root/path)]
 subprocess.run(command+[str(here/'check.cpp'),str(worker),'-o',str(binary)],check=True)
 try:r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=DEADLOCK_GUARD)
 except subprocess.TimeoutExpired as hung:
  steps=(hung.stdout or b'').decode(errors='replace').strip().splitlines()
  sys.exit(f'FAIL: no progress for {DEADLOCK_GUARD} s (a generator deadlock) at {steps[-1] if steps else "start"}')
 if a.baseline_revision:
  assert r.returncode==23 and 'SECOND_COMPUTE_DID_NOT_OVERLAP' in r.stderr,(r.returncode,r.stdout,r.stderr)
  print('old actual generator compiler0, exact SECOND_COMPUTE_DID_NOT_OVERLAP runtime negative PASS')
 else:
  assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
  print(r.stdout,end='')
