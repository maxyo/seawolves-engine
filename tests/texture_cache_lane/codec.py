#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2];fx=root/'src/libs/renderer_bgfx/fx';here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='cache-codec-') as tmp:
 p=Path(tmp);upscale=fx/'src/upscale.cpp';flags=[]
 if '--baseline' in sys.argv:
  upscale=p/'old-upscale.cpp';s=subprocess.check_output(['git','show','0e14ee06:src/libs/renderer_bgfx/fx/src/upscale.cpp'],cwd=root,text=True)
  # Match only the harmless current optional-parameter signature, keep old body.
  s=s.replace('uint64_t key, std::string *error)','uint64_t key, std::string *error, uint64_t)');upscale.write_text(s)
  # Run rename regression first, old reader reservation semantics intentionally untested.
  check=(here/'codec.cpp').read_text();start=check.index(' std::string error;');end=check.index(' auto badtarget',start);check=check[:start]+' std::string error;'+check[end:];check=check[:check.index(' std::ofstream(UpscaleFile')]+ '\n}\n';source=p/'old-check.cpp';source.write_text(check)
 else:source=here/'codec.cpp'
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-O0','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(fx/'include'),str(source),str(upscale),str(fx/'src/relief_store.cpp'),str(fx/'src/normal_map.cpp'),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe),str(p)],capture_output=True,text=True,timeout=4)
 if '--baseline' in sys.argv:
  assert r.returncode!=0 and 'rename failure must not report success after cleanup' in r.stderr,r.stderr
  print('PASS actual old WriteUpscaled semantic negative')
 else:
  assert r.returncode==0,r.stderr
  print(r.stdout,end='')
