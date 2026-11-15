#!/usr/bin/env python3
"""Actual scene_complete target transition followed by actual Present probe check.
CPU production latch/gate; GPU post/copy calls are explicit inert test doubles.
"""
from pathlib import Path
import subprocess, tempfile, sys, os
root=Path(__file__).resolve().parents[2]
path='src/libs/renderer_bgfx/src/shim/device.cpp'
s=(root/path).read_text()
if '--baseline' in sys.argv:
    s=subprocess.check_output(['git','show','06683035:'+path],cwd=root,text=True)
start=s.index('    // The interface is drawn on the processed picture')
tail=s[start:s.index('\n}\n',start)]
start=s.index('    if (views_used > 0 && !render::HideSceneBeauty')
probe=s[start:s.index('    probe.before_frame();',start)]
start=s.index('    render::BeginSceneWork(')
begin_bridge=s[start:s.index('    return D3D_OK;',start)]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <utility>
#include <initializer_list>
#include "storm/render/loading_scene_work.hpp"
#include "effects/scene_readiness.hpp"
using storm::render::LoadingSceneWork;
using storm::effects::SceneReadiness;
LoadingSceneWork work; uint64_t gen=7;
namespace bgfx {using ViewId=uint16_t;}
struct Context {struct S {uint64_t generation=5;} surface;
 uint64_t frame=0;uint64_t last_frame(){return frame;}
 static Context& instance(){static Context c;return c;} S& Surface(){return surface;}};
namespace render {
 constexpr int kGameViewBegin=0;
 void BeginSceneWork(uint64_t f,uint64_t s,uint64_t t,bool stable){work.begin(f,gen,s,t,true,true,false,stable);}
 bool HideSceneBeauty(uint64_t s,uint64_t t){return work.hidden(gen,s,t);}
 void ScenePostTarget(uint64_t s,uint64_t a,uint64_t b){work.post_target(gen,s,a,b);}
}
struct Store {uint64_t serial;int handle=0;int width=1280,height=720;};
using TexStore=Store;
struct Ptr {Store* p=nullptr; Store* get(){return p;} Store* operator->(){return p;} operator bool(){return p;}};
struct Color {Ptr store;};
struct Host {template<class...T>void copy_scene_depth(T...) {}};
struct Probe {void place(int){}};
struct Device {
 Store a{8},b{9},c{10},depth{11};Color color{{&a}},depth_color{{&depth}};
 Color*back_color=&color;Color*back_depth=&depth_color;Ptr post_color{&b},held_color{},held_depth{&depth};
 int views_used=2;bool view_dirty=false,backbuffer_recreated=false;Host host;Probe probe;
 int framebuffer(Store*,int,int,Store*){return 0;}
 void leave_scaled_picture(bool copy,bool kept){assert(!copy&&kept);back_color->store=held_color;held_color={};}
 void begin_scene(){
 BEGIN_BRIDGE
 }
 void post(bool scaled){
 POST
 }
 void present_probe(){ auto &context=Context::instance();
 PROBE
 }
};
void begin(Device&d,int frame,bool scaled=false){
 Context::instance().frame=frame;d.begin_scene();
 work.beauty_submitted();
}
int main(){
 for(bool scaled:{false,true}) {
  SceneReadiness gate;gen=gate.begin(0);Device d;
  for(int i=1;i<=3;++i){
   if(scaled){d.held_color=d.back_color->store;d.back_color->store={&d.c};}
   begin(d,i,scaled);d.post(scaled);d.present_probe();work.presented(true,true);
   assert(work.complete(gen) && "actual post swap followed by Present must qualify");
   gate.frame(gen,i,work.complete(gen),{});
   assert((gate.state()==SceneReadiness::State::Ready)==(i==3));work.end();
  }
 }
 Device d;gen=7;
 // Ordinary no-post path is still qualified.
 begin(d,10);d.present_probe();work.presented(true,true);assert(work.complete(gen));work.end();
 // Unowned target replacement, surface/generation change and failed Present remain rejected.
 for(int kind=0;kind<4;++kind){
  Device x;Context::instance().surface.generation=5;begin(x,20+kind);x.post(false);
  if(kind==0)x.back_color->store={&x.c};
  if(kind==1)Context::instance().surface.generation=6;
  if(kind==2)++gen;
  x.present_probe();work.presented(kind!=3,true);assert(!work.complete(gen));work.end();gen=7;
 }
 Context::instance().surface.generation=5;
 // A sanctioned handoff must never heal earlier revocation, dirtiness or wrong old target.
 for(int kind=0;kind<3;++kind){
  Device x;begin(x,30+kind);
  if(kind==0)work.hidden(gen,6,8);
  if(kind==1)work.demand();
  if(kind==2)x.back_color->store={&x.c};
  x.post(false);x.present_probe();work.presented(true,true);assert(!work.complete(gen));work.end();
 }
}
'''.replace(' POST',tail).replace(' PROBE',probe).replace(' BEGIN_BRIDGE',begin_bridge)
with tempfile.TemporaryDirectory(prefix='post-handoff-') as tmp:
    p=Path(tmp)/'check.cpp';p.write_text(fixture);binary=Path(tmp)/'check'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O0','-g','-fsanitize=address,undefined','-I'+str(root/'src/libs/renderer_bgfx/include'),'-I'+str(root/'src/libs/renderer_bgfx/src'),str(p),'-o',str(binary)],check=True)
    r=subprocess.run([str(binary)],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
    print(r.stdout+r.stderr,end='')
    if '--baseline' in sys.argv:
        assert r.returncode != 0 and 'actual post swap followed by Present must qualify' in r.stderr, 'baseline must fail semantic assertion, not compile/sanitizer'
        print('actual baseline post-swap→Present assertion negative PASS')
    else:
        assert r.returncode==0,r.returncode
        print('actual post-swap→Present +3full/scaled/no-post/invalidation controls PASS')
