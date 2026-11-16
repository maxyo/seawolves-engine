#!/usr/bin/env python3
"""Production ProgressView/RunEnd bodies; drawing and Present are inert counters."""
from pathlib import Path
import subprocess, tempfile, sys, os
root = Path(__file__).resolve().parents[2]
path = 'src/libs/renderer/src/renderer.cpp'
s = (root / path).read_text()
def body(signature):
    start = s.index(signature)
    opening = s.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
# Both direct script progress and TextureLoad's production entry use this method.
assert 'ProgressView();' in body('bool Renderer::TextureLoad(int32_t t)')
fixture = r'''
#include <cassert>
#include <cstdint>
namespace storm {
namespace loading {bool active=false, ready=false, first_frame=true;}
namespace render {void SceneLoadingTransactionFrame(){} bool HoldSceneReveal(){return true;}}
}
struct Core {int errors=0; void Trace(const char *){++errors;}} core;
struct Renderer {
 int progressTexture=4, progressSafeCounter=0;
 bool bInsideScene=true;
 int target=99, viewport=128, draws=0, presents=0, releases=0;
 void ProgressView();void RunEnd();
 void DrawProgressView(bool present){++draws;if(present){++presents;target=0;viewport=720;}}
 void DrawFrameStats(){++presents;}
 void EndProgressView(){++releases;progressTexture=-1;}
};
PROGRESS
RUNEND
int main(){
 Renderer r;
 storm::loading::active=true;
 r.ProgressView(); // lazy texture request while a reflection target is bound
 assert(r.presents==0 && "nested loading progress must not Present");
 assert(r.draws==0 && r.target==99 && r.viewport==128 && r.progressTexture==4);
 r.RunEnd();
 assert(r.presents==1 && r.draws==1 && r.releases==0 && !storm::loading::ready);
 assert(r.target==99 && r.viewport==128 && r.progressTexture==4);
 storm::loading::active=false;
 r.ProgressView();
 assert(r.presents==2 && r.draws==2 && "ordinary progress still presents");
}
'''.replace('PROGRESS', body('void Renderer::ProgressView()')).replace('RUNEND', body('void Renderer::RunEnd()'))
with tempfile.TemporaryDirectory(prefix='nested-progress-') as tmp:
    source=Path(tmp)/'check.cpp';binary=Path(tmp)/'check';source.write_text(fixture)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O0',str(source),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True)
    assert result.returncode == 0, result.stderr
    print('production nested/ordinary progress + loading RunEnd controls PASS')
