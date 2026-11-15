#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
here=Path(__file__).resolve().parent
fx=here.parents[1]/'src/libs/renderer_bgfx/fx'
with tempfile.TemporaryDirectory(prefix='sea-optics-') as out:
    binary=str(Path(out)/'check')
    cmd=['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
         '-I'+str(fx/'include'),str(here/'check.cpp'),str(fx/'src/settings.cpp'),'-o',binary]
    if '--legacy' in sys.argv:cmd+=['-DLEGACY_TRANSMISSION']
    subprocess.run(cmd,check=True)
    result=subprocess.run([binary]).returncode
    if result or '--legacy' in sys.argv:sys.exit(result)
    source=(here.parents[1]/'src/libs/renderer_bgfx/src/effects/world.cpp').read_text()
    start=source.index('void Host::prepare_sea(');brace=source.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    (Path(out)/'actual_prepare_sea.hpp').write_text(source[start:end])
    host=str(Path(out)/'host')
    subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
                    '-I'+str(fx/'include'),'-I'+out,str(here/'host_check.cpp'),'-o',host],check=True)
    sys.exit(subprocess.run([host]).returncode)
