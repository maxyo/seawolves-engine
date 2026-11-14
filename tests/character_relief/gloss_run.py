#!/usr/bin/env python3
"""CPU controls for the production torso-mask gate and specular-only shader block; no GPU."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--only', choices=('gate', 'shader', 'all'), default='all')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
renderer = root / 'src/libs/renderer_bgfx'
lighting = Path(os.environ.get('STORM_CHARACTER_LIGHTING_SOURCE', str(renderer / 'src/effects/lighting.cpp'))).read_text()
fragment = Path(os.environ.get('STORM_CHARACTER_FRAGMENT_SOURCE', str(renderer / 'shaders/include/ff_fragment.sh'))).read_text()
call = lighting.index('draw.skin_tone = skin_tone(')
start = lighting.rfind('if (', 0, call)
condition = lighting[start + 4:call].strip()
assert condition.endswith(')')
condition = condition[:-1]
start = fragment.index('    float ffSkin = clamp(')
end = fragment.index('    // The highlight of this pixel:', start)
# FX_LIGHT_NORMALMAP intentionally remains undefined: this is the production specular-only variant.
block = fragment[start:end]
source = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using std::clamp;
double max(double a,double b){return std::max(a,b);}
void expect(bool value,const char *what) {if(!value){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}std::puts(what);}
namespace bgfx { bool isValid(bool handle){return handle;} }
struct Handle { bool handle; };
struct Layout { bool skinned; };
struct Settings { bool character_skin_colour; };
bool prepare(bool has_relief,bool gloss,bool character=true,bool skinned=true,bool albedo=true,bool enabled=true) {
    [[maybe_unused]] Handle gloss_map{gloss}; Layout layout{skinned}; Settings settings_{enabled};
    [[maybe_unused]] const bool stage0=albedo;
    [[maybe_unused]] const int relief_stage=has_relief?0:-1;
    return CONDITION;
}
struct Uniform { double x=0,w=0; };
struct Vertex { double w=0; };
struct UV { double xy=0; };
struct Texel { double rgb; };
double sampled_likeness=0;
Texel texture2DLod(int,double,double){return {sampled_likeness};}
double ffSkinLikeness(double value){return value;}
double mask(double head,double torso,double reference,double likeness) {
    const Vertex v_normal{head};
    [[maybe_unused]] const Vertex v_position{torso};
    [[maybe_unused]] const UV v_texcoord0{};
    [[maybe_unused]] const Uniform u_fxSkinTone[2]={{reference,0},{0,0}};
    [[maybe_unused]] const int s_tex0=0;
    sampled_likeness=likeness;
BLOCK
    return ffSkin;
}
int main(){
GATE_CASES
SHADER_CASES
    std::puts("PASS character specular-only torso protection");
}
'''.replace('CONDITION', condition).replace('BLOCK', block)
gate_cases = r'''
    expect(prepare(true,false),"normal-ready reference retained");
    expect(prepare(false,true),"gloss-only draw acquires torso reference even with normal strength zero");
    expect(!prepare(false,false),"no generated maps need no reference");
    expect(!prepare(false,true,false),"non-character draws unchanged");
    expect(!prepare(false,true,true,false),"unskinned draws unchanged");
    expect(!prepare(false,true,true,true,false),"missing albedo remains safe");
    expect(!prepare(false,true,true,true,true,false),"explicit colour-mask disable retained");
'''
shader_cases = r'''
    expect(mask(0,1,-1,0)==1,"unknown torso protected without FX_LIGHT_NORMALMAP");
    expect(mask(0,1,.4,1)==1,"known bare torso protected in gloss-only shader");
    expect(mask(0,1,.4,0)==0,"non-skin clothing retains generated gloss");
    expect(mask(0,1,0,1)==0,"disabled colour mask remains disabled");
    expect(mask(.5,0,0,0)==1,"head and hand bone protection unchanged");
    expect(mask(0,.25,-1,0)==.5,"unknown partial torso blend retained");
'''
source = source.replace('GATE_CASES', gate_cases if args.only != 'shader' else '').replace('SHADER_CASES', shader_cases if args.only != 'gate' else '')
with tempfile.TemporaryDirectory(prefix='storm-character-gloss-') as directory:
    cpp = Path(directory) / 'check.cpp'
    binary = Path(directory) / 'check'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
