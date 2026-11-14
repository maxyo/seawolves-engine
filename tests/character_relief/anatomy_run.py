#!/usr/bin/env python3
"""Actual vertex mask encoding: unknown anatomy suppresses relief; known bones retain their exact weights."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
text=Path(os.environ.get('STORM_CHARACTER_SKINNING_SOURCE',str(root/'src/libs/model/include/gpu_skinning.hpp'))).read_text()
start=text.index('static void writeSkinWeights(');brace=text.index('{',start);depth=1;i=brace+1
while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
function=text[start:i].replace('static void writeSkinWeights(','void writeSkinWeights(',1)
source='''#include "storm/render/skinned_vertex.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace storm::render;
void expect(bool v,const char *s){if(!v){std::fprintf(stderr,"FAIL %s\\n",s);std::exit(1);}std::puts(s);}
'''+function+'''
int main(){
 uint8_t v[kSkinnedSourceStride]{};float w=0.5f;std::memcpy(v+kSkinnedWeightOffset,&w,sizeof(w));
 v[kSkinnedBonesOffset]=0;v[kSkinnedBonesOffset+1]=1;
 writeSkinWeights(v,1,{});
 expect(v[kSkinnedBonesOffset+kSkinWeightByte]==255&&v[kSkinnedBonesOffset+kTorsoWeightByte]==0,"unsupported skeleton encoded as protected skin independently of colour reference");
 writeSkinWeights(v,1,{1,2,0});
 expect(v[kSkinnedBonesOffset+kSkinWeightByte]==128&&v[kSkinnedBonesOffset+kTorsoWeightByte]==128,"known head/torso blend retains exact previous masks");
 v[kSkinnedBonesOffset]=v[kSkinnedBonesOffset+1]=2;writeSkinWeights(v,1,{1,2,0});
 expect(v[kSkinnedBonesOffset+kSkinWeightByte]==0&&v[kSkinnedBonesOffset+kTorsoWeightByte]==0,"known unmasked clothing bones remain unchanged");
 v[kSkinnedBonesOffset]=v[kSkinnedBonesOffset+1]=255;writeSkinWeights(v,1,{1,2,0});
 expect(v[kSkinnedBonesOffset+kSkinWeightByte]==255,"out-of-range anatomy never silently unmasks character skin");
 std::puts("PASS conservative unknown anatomy masks");
}
'''
with tempfile.TemporaryDirectory(prefix='storm-character-anatomy-') as d:
    p=Path(d)/'check.cpp';b=Path(d)/'check';p.write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-I'+str(root/'src/libs/renderer_bgfx/include'),str(p),'-o',str(b)],check=True)
    subprocess.run([str(b)],check=True)
