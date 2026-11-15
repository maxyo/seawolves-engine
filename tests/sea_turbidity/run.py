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
    if '--legacy-camera' in sys.argv:cmd+=['-DLEGACY_CAMERA']
    subprocess.run(cmd,check=True)
    result=subprocess.run([binary]).returncode
    if result or '--legacy' in sys.argv:sys.exit(result)
    source=(here.parents[1]/'src/libs/renderer_bgfx/src/effects/world.cpp').read_text()
    start=source.index('void Host::prepare_sea(');brace=source.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    production=source[start:end]
    if '--legacy-attachment' in sys.argv:
        production=production.replace('shim::TexStore *depth = color ? device.view_depth : nullptr;',
                                      'shim::TexStore *depth = device.ds ? device.ds->store.get() : nullptr;')
    (Path(out)/'actual_prepare_sea.hpp').write_text(production)
    host=str(Path(out)/'host')
    subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
                    '-I'+str(fx/'include'),'-I'+out,str(here/'host_check.cpp'),'-o',host],check=True)
    result=subprocess.run([host]).returncode
    if result:sys.exit(result)
    def extract(path,signature):
        text=path.read_text();start=text.index(signature);end=text.index('{',start)+1;depth=1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]
    triangle=extract(fx/'src/gpu_util.cpp','bool SubmitScreenTriangle(')
    capture=extract(fx/'src/sea_depth.cpp','bool SeaDepth::Capture(')
    if '--legacy-capture' in sys.argv:
        capture=capture.replace('return SubmitScreenTriangle(view, program_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);',
                                'SubmitScreenTriangle(view, program_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A); return true;')
    (Path(out)/'actual_capture.hpp').write_text(triangle+'\n'+capture)
    binary=str(Path(out)/'capture')
    subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
                    '-I'+out,str(here/'capture_check.cpp'),'-o',binary],check=True)
    sys.exit(subprocess.run([binary]).returncode)
