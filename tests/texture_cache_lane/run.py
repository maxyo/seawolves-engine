#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2];here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='cache-lane-') as tmp:
 p=Path(tmp);src=root/'src/libs/renderer_bgfx/src';source=src/'texture/core/service.cpp';includes=[];flags=[]
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-pthread','-O0','-fno-access-control','-Wall','-Wextra','-Werror',*flags,*includes,'-I'+str(root/'tests/texture_service/stubs'),'-I'+str(src),str(here/'check.cpp'),str(source),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8)
 assert r.returncode==0,r.stderr
 print(r.stdout,end='')
