#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2];fx=root/'src/libs/renderer_bgfx/fx';here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='cache-codec-') as tmp:
 p=Path(tmp);upscale=fx/'src/upscale.cpp'
 source=here/'codec.cpp'
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-O0','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(fx/'include'),str(source),str(upscale),str(fx/'src/relief_store.cpp'),str(fx/'src/normal_map.cpp'),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe),str(p)],capture_output=True,text=True,timeout=60)
 assert r.returncode==0,r.stderr
 print(r.stdout,end='')
