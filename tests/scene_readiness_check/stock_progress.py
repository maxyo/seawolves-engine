#!/usr/bin/env python3
"""Exercise extracted production stock progress lifecycle without renderer/GPU dependencies."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
renderer = (root/'src/libs/renderer/src/renderer.cpp').read_text()
def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1; end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]
methods = '\n'.join(function(renderer, 'void Renderer::'+name+'()') for name in
    ['StartProgressView','RunEnd','EndProgressView','ReleaseProgressView'])
sun = function((root/'src/libs/renderer_bgfx/src/effects/shadows.hpp').read_text(), 'int initial_state(')
source = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
namespace storm::render {
struct Status { bool active=false; }; Status status;
bool pending=false; int seals=0;
Status GetSceneLoadStatus(){return status;}
bool HoldSceneReveal(){return pending;}
void CompleteSceneReveal(){assert(!pending);status.active=false;++seals;}
}
uint32_t SDL_GetTicks(){return 1000;}
struct Core {void Trace(const char*){}} core;
struct Renderer {
 int progressTexture=-1,back0Texture=-1,backTexture=-1,progressTipsTexture=-1;
 int progressSafeCounter=0,loadFrame=0; uint32_t progressUpdateTime=0;
 bool progressEndRequested=false,isInPViewProcess=false,bInsideScene=true;
 char image[32]="first",back[32]="background",tips[32]="tip";
 char *progressImage=image,*progressBackImage=back,*progressTipsImage=tips;
 int progressImageSize=32,progressBackImageSize=32,progressTipsImageSize=32;
 int next=0,overlay=0,synchronous=0; std::vector<int> released; std::vector<std::string> created;
 int TextureCreate(const char*p){created.emplace_back(p);return ++next;}
 void TextureRelease(int id){released.push_back(id);}
 void DrawFrameStats(){}
 void DrawProgressView(bool present){assert(!present);++overlay;}
 void ProgressView(){++synchronous;}
 void StartProgressView(); void RunEnd(); void EndProgressView(); void ReleaseProgressView();
};
namespace fx {float SunShadowStrength(float y){return y>0?1:0;}}
namespace bgfx {bool isValid(int h){return h>=0;}}
struct Sun {
 bool applicable=true,on=true,sun_present_=true,prepared_=false,active_=false;
 float sun_dir_[3]={0,1,0};std::vector<int> caster_indices_;bool tiles_dirty_=false,tiles_failed_=false;int tiles_vb_=-1;
 bool enabled()const{return on;}
SUN_METHOD
};
METHODS
int main(){
 using namespace storm::render;
 Renderer r;r.StartProgressView();status.active=true;pending=true;
 const int sprite=r.progressTexture,oldBack=r.backTexture,oldTip=r.progressTipsTexture;
 r.EndProgressView();assert(r.progressTexture==sprite&&r.progressEndRequested);
 for(int i=0;i<80;++i)r.RunEnd();
 assert(r.overlay==80&&r.synchronous==0&&seals==0&&r.released.empty()); // no Fader; both legacy exits blocked
 // Resource changes/failure remain a real hold, never a timeout reveal.
 r.RunEnd();assert(r.progressTexture>=0&&seals==0);
 std::string("second").copy(r.image,7);r.tips[0]=0;r.StartProgressView();
 assert(!r.progressEndRequested&&r.progressTexture==sprite&&r.progressTipsTexture==-1);
 assert(std::find(r.released.begin(),r.released.end(),oldBack)!=r.released.end());
 assert(std::find(r.released.begin(),r.released.end(),oldTip)!=r.released.end());
 assert(r.created.back()=="second"); // new generation artwork, no stale old tip
 r.EndProgressView();pending=false;r.RunEnd();assert(seals==1&&r.progressTexture==-1);
 Renderer cancel;cancel.StartProgressView();status.active=true;pending=true;cancel.EndProgressView();
 status.active=false;cancel.RunEnd();assert(cancel.progressTexture==-1&&cancel.synchronous==0); // sealed cancel
 Renderer teardown;teardown.StartProgressView();status.active=true;pending=true;teardown.ReleaseProgressView();
 assert(teardown.progressTexture==-1&&teardown.backTexture==-1); // unconditional destruction despite pending
 Renderer legacy;status.active=false;legacy.StartProgressView();for(int i=0;i<51;++i)legacy.RunEnd();
 assert(legacy.progressTexture==-1&&legacy.synchronous==51);
 Sun sun;assert(sun.initial_state(false)==0);assert(sun.initial_state(true)==1); // indoor daylight vs outdoor required
 sun.prepared_=true;assert(sun.initial_state(true)==2); // failed outdoor target cannot reveal
 sun.active_=true;assert(sun.initial_state(true)==0);
 sun.caster_indices_.push_back(1);sun.tiles_dirty_=true;assert(sun.initial_state(true)==1);
 sun.tiles_dirty_=false;assert(sun.initial_state(true)==2);sun.tiles_vb_=1;assert(sun.initial_state(true)==0);
}
'''.replace('SUN_METHOD',sun).replace('METHODS',methods)
with tempfile.TemporaryDirectory(prefix='stock-progress-check-') as tmp:
    p=Path(tmp);(p/'check.cpp').write_text(source)
    subprocess.run(['c++','-std=c++17','-O0','-fsanitize=address,undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('Stock progress lifecycle and indoor/outdoor sun controls PASS')
