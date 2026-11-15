#!/usr/bin/env python3
"""Local provenance fixture, not a standard suite target: actual Finalizer vs verbatim old tail.
Requires --warm-inputs with explicitly pinned existing host codec archives. No models/GPU/device.
"""
from pathlib import Path
import argparse,os,subprocess,tempfile,hashlib,json
p=argparse.ArgumentParser();p.add_argument('--warm-inputs',required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];here=Path(__file__).resolve().parent
pins=json.loads(Path(a.warm_inputs).read_text());build=Path(pins['build'])
for item in pins['inputs']:
 if 'libstorm_relief_core' not in item['path'] and 'libstorm_bgfx_fx_core' not in item['path']:
  assert hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256'],item['path']
# The only reused engine members are unchanged encoder/formats/mips, never old Service or old fx.
closure=['eac_encoder.cpp','eac_encoder.hpp','formats.cpp','formats.hpp','mips.cpp','mips.hpp','cache.cpp','cache.hpp']
for file in closure:
 path='src/libs/renderer_bgfx/src/texture/core/'+file
 if (root/path).exists():assert not subprocess.check_output(['git','diff','25686db2872135da1ca8508f87d3143ae6376bf7','--',path],cwd=root),path
source=(root/'src/libs/relief/prepare.cpp').read_text()
old=subprocess.check_output(['git','show','0e14ee0618272e0e94585ba273739bef5267a819:src/libs/relief/prepare.cpp'],cwd=root,text=True)
tail=old[old.index('    // The models\' planes at the map\'s size'):old.index('\n}\n\nOutcome Session::Upscale',old.index('    // The models\' planes at the map\'s size'))]
helpers=source[source.index('std::vector<uint8_t> HalveTo('):source.index('// A grey')]
write=old[old.index('bool WriteDisplay('):old.index('\n} // namespace',old.index('bool WriteDisplay('))]
assert write==source[source.index('bool WriteDisplay('):source.index('\n} // namespace',source.index('bool WriteDisplay('))], 'WriteDisplay helper changed since baseline'
helpers='double Seconds(Clock::time_point t){return std::chrono::duration<double>(Clock::now()-t).count();}\n'+helpers+write
upscale=(root/'src/libs/renderer_bgfx/fx/src/upscale.cpp').read_text()
label=upscale[upscale.index('const char *UpscaleTargetName('):upscale.index('\n}',upscale.index('const char *UpscaleTargetName('))+2]
helpers+='\nnamespace storm::fx {\n'+label+'\n}\n'
text=(here/'check.cpp').read_text().replace('// EXTRACTED_HELPERS',helpers).replace('// EXTRACTED_OLD_TAIL',tail)
import re
# Read the pinned build's actual dependency includes rather than assuming workspace paths.
ninja=(build/'build.ninja').read_text();incs=re.findall(r'-I([^\s]+/include)',ninja)
bxinclude=next(Path(v) for v in incs if '/storm_bx-' in v)
bimginclude=next(Path(v) for v in incs if '/storm_bimg-' in v)
includes=[root/'src/libs/relief',root/'src/libs/renderer_bgfx/fx/include',root/'src/libs/renderer_bgfx/src/texture/core',bimginclude,bxinclude]
with tempfile.TemporaryDirectory(prefix='relief-finalize-',dir=os.environ.get('TMPDIR')) as directory:
 d=Path(directory);check=d/'check.cpp';check.write_text(text)
 command=[os.environ.get('CXX','c++'),'-std=c++20','-O0','-g','-pthread','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']+['-I'+str(v) for v in includes]
 sources=[check,root/'src/libs/relief/prepare.cpp']+[root/('src/libs/renderer_bgfx/fx/src/'+v+'.cpp') for v in ['normal_map','relief_model','height_map','gloss_map','parallax_trust']]
 plant=root/'src/libs/renderer_bgfx/fx/src/plant_relief.cpp'
 if plant.exists():sources.append(plant)
 objects=[]
 for n,src in enumerate(sources):
  obj=d/(str(n)+'.o');subprocess.run(command+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
 libs=[build/'renderer_bgfx/libstorm_texture_core.a']+[build/('_deps/storm_bgfx_cmake-build/cmake/'+v) for v in ['bimg/libbimg_encode.a','bimg/libbimg.a','bx/libbx.a']]
 binary=d/'check';linkmap=d/'providers.map'
 subprocess.run(command+list(map(str,objects))+list(map(str,libs))+['-Wl,--gc-sections','-Wl,-Map='+str(linkmap),'-lrt','-ldl','-o',str(binary)],check=True)
 providers=linkmap.read_text();assert 'eac_encoder.cpp.o' in providers and 'formats.cpp.o' in providers
 assert 'libstorm_bgfx_fx_core' not in providers and 'libstorm_relief_core' not in providers
 r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15);print(r.stdout,end='')
 if r.returncode:print(r.stderr)
 assert r.returncode==0,r.returncode
 print('provider map: current explicit prepare/fx objects; unchanged warm EAC/formats/bimg archives only')
