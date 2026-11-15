#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2];here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='cache-lane-') as tmp:
 p=Path(tmp);src=root/'src/libs/renderer_bgfx/src';source=src/'texture/core/service.cpp';includes=[];flags=[]
 if '--baseline' in sys.argv:
  tree=p/'texture/core';tree.mkdir(parents=True)
  for name in ['service.cpp','service.hpp','cache.hpp','formats.hpp']:
   (tree/name).write_bytes(subprocess.check_output(['git','show','0e14ee06:src/libs/renderer_bgfx/src/texture/core/'+name],cwd=root))
  source=tree/'service.cpp';includes=['-I'+str(p)];flags=['-DBASELINE']
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-pthread','-O0','-fno-access-control','-Wall','-Wextra','-Werror',*flags,*includes,'-I'+str(root/'tests/texture_service/stubs'),'-I'+str(src),str(here/'check.cpp'),str(source),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8)
 if '--baseline' in sys.argv:
  assert r.returncode!=0 and 'FAIL: cache IO must finish before held transcode release' in r.stderr,r.stderr
  print('PASS actual old scheduler negative: cache IO blocked by transcode')
 else:
  assert r.returncode==0,r.stderr
  print(r.stdout,end='')
