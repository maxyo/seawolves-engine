#!/usr/bin/env python3
"""CPU production character eligibility/caching/binding controls; no GPU/models."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
fx=root/'src/libs/renderer_bgfx/fx'
effects=root/'src/libs/renderer_bgfx/src/effects'
def function(path, declaration):
    text=Path(path).read_text();start=text.index(declaration);brace=text.index('{',start);depth=1;i=brace+1
    while depth:
        depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[start:i]
gate=function(effects/'normal_maps.cpp','bool NormalMaps::parallax_allowed(')
with tempfile.TemporaryDirectory(prefix='storm-character-relief-') as directory:
    directory=Path(directory);source=directory/'check.cpp';binary=directory/'check'
    source.write_text('''#include "storm/fx/height_map.hpp"
#include "storm/fx/parallax_exclusions.hpp"
#include "storm/fx/parallax_trust.hpp"
#include <cstdio>
#include <cstdlib>
using namespace storm;
void expect(bool value,const char *what) {if(!value){std::fprintf(stderr,"FAIL %s\\n",what);std::exit(1);}std::puts(what);}
struct NormalMapEntry {std::string name;fx::ReliefClass relief;float trust_coverage;};
struct NormalMaps {
 bool exclusions_loaded_=true; fx::ParallaxExclusions exclusions_;
 void load_exclusions(){std::abort();} bool parallax_allowed(const NormalMapEntry &);
};
'''+gate+'''
int main(int argc,char **argv) {
 expect(argc==2,"fixture directory");NormalMaps maps;
 NormalMapEntry character{"characters/test",fx::ReliefClass::Character,1.0f};
 expect(maps.parallax_allowed(character),"default trusted character eligible through actual NormalMaps gate");
 character.trust_coverage=fx::kParallaxMinCoverage-0.001f;
 expect(!maps.parallax_allowed(character),"low-trust character remains rejected");
 character.trust_coverage=fx::kParallaxMinCoverage;
 expect(maps.parallax_allowed(character),"threshold character eligible");
 maps.exclusions_.LoadShipped("exclude characters/test\\n");
 expect(!maps.parallax_allowed(character),"shipped exclusion still rejects character");
 expect(maps.exclusions_.LoadUser(std::filesystem::path(argv[1])/"user.txt"),"load user overrides");
 expect(maps.exclusions_.SetUser(character.name,true),"user include persisted");
 expect(maps.parallax_allowed(character),"explicit user include retains existing shipped override");
 expect(maps.exclusions_.SetUser(character.name,false),"user exclusion persisted");
 expect(!maps.parallax_allowed(character),"explicit user exclusion still rejects character");
 for(auto relief:{fx::ReliefClass::Wood,fx::ReliefClass::Prop,fx::ReliefClass::Plaster,fx::ReliefClass::Metal,fx::ReliefClass::HeightMap,fx::ReliefClass::None})
  expect(!maps.parallax_allowed({"other",relief,1.0f}),"unrelated class default unchanged");
 for(auto relief:{fx::ReliefClass::Stone,fx::ReliefClass::Building})
  expect(maps.parallax_allowed({"other",relief,1.0f}),"masonry default preserved");
 std::puts("PASS character relief gate");
}
''')
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-Wall','-Wextra','-Werror',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(fx/'include'),str(source),os.environ.get('STORM_CHARACTER_HEIGHT_SOURCE',str(fx/'src/height_map.cpp')),
        str(fx/'src/parallax_exclusions.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(directory)],check=True)

subprocess.run([os.environ.get('PYTHON','python3'),str(Path(__file__).with_name('cache_run.py'))],check=True)
subprocess.run([os.environ.get('PYTHON','python3'),str(Path(__file__).with_name('binding_run.py'))],check=True)
subprocess.run([os.environ.get('PYTHON','python3'),str(Path(__file__).with_name('anatomy_run.py'))],check=True)

subprocess.run([os.environ.get('PYTHON','python3'),str(Path(__file__).with_name('gloss_run.py'))],check=True)
