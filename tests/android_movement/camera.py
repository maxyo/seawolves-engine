#!/usr/bin/env python3
"""Check the real follow camera (mobile camera mode): drag yaw, auto-follow, tight spaces (tightView), hero fade, dialogue framing; --source-root supports baseline replay."""
import argparse,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[2]);a=p.parse_args()
s=(a.source_root/'src/libs/location/src/camera_follow.cpp').read_text()
start=s.index('void CameraFollow::MoveCamera(');end=s.index('// Reinitialize camera position',start)
code=r'''
#include <cassert>
#include <cmath>
#include <platform/android_camera.hpp>
#include <cstdint>
#include <iostream>
#include <vector>
struct Vec3 {float x=0,y=0,z=0;Vec3 operator-(const Vec3&o)const{return {x-o.x,y-o.y,z-o.z};}};
namespace glm{using vec3=Vec3;}
struct Character {bool fight=false,locked=false;bool MobileCameraOrbit()const{return !fight;}void LockRotate(bool value){locked=value;}};
struct LocationCamera {float chay=0,dAy=0,dragAy=0,dragAx=0,lheight=1,rotInertia=8,ax=.4f,radius=3;bool isSpecialMode=false;Vec3 pos;Character *character;};
struct CameraFollow {
 LocationCamera *lc;float camay=0,camradius=3,kRadInert=0,tightRaise=0,tightAge=0;bool settle_=false;
 Vec3 lookTo,camPos,heroPos;storm::android::AutoFollow autoFollow;storm::android::Damped raise,swing;
 float wall=3,ceiling=3,low=-1,openYaw=99,raised=0,maxPitch=-9;
 void MoveCamera(float);void MoveMobile(float);
 // Walls `wall` metres behind the hero; raised over him the room is `ceiling`, lowered `low` (else `wall`); more towards `openYaw`.
 float FindRadius(float yaw,float pitch){maxPitch=std::fmax(maxPitch,pitch);
  return std::fmax(pitch>lc->ax+.1f?ceiling:pitch<lc->ax-.1f&&low>=0?low:wall,3-4*std::fabs(yaw-openYaw));}
 void ChangeRadius(float,float r){camradius=r;}
 void CalcPosition(float,float,float dax,Vec3&){raised=-dax;}
};
''' + s[start:end] + r'''
int main(){
 using namespace storm::android;
 Character actor;LocationCamera lc;lc.character=&actor;CameraFollow camera{&lc};
 const float pi=3.14159265f;
 // Standing, the stick may turn the hero any way: the camera stays (no idle recentring), never locks him.
 for(int step=0;step<720;++step){lc.chay=step*.017453292f;camera.MoveCamera(.016f);assert(camera.camay==0&&!actor.locked);}
 // A drag turns the view by all of it at any frame time; across the whole width: half a turn, scaled by the sensitivity.
 for(float dt:{.016f,.1f,1.f}){const float before=camera.camay;lc.dragAy=dragYaw(64,1280);camera.MoveCamera(dt);assert(std::fabs(camera.camay-before-pi/20)<1e-5f);}
 float total=0;for(int i=0;i<16;++i)total+=dragYaw(80,1280);assert(std::fabs(total-pi)<1e-4f);
 assert(std::fabs(dragYaw(2400,2400)-pi)<1e-5f&&std::fabs(dragYaw(1200*2.f,2400)-pi)<1e-5f&&dragYaw(5,0)==0&&dragYaw(NAN,1280)==0);
 lc.dragAy=0;
 // Damped: critically damped (never past the target, no oscillation), the same path at any frame rate.
 {Damped a{1},b{1};float last=1;for(int i=0;i<240;++i){a.step(0,1.f/60,.8f);assert(a.value>=-1e-6f&&a.value<=last+1e-6f);last=a.value;}
  for(int i=0;i<40;++i)b.step(0,.1f,.8f);assert(std::fabs(a.value-b.value)<1e-4f&&a.value<.1f);}

 // A walk: the hero moves at `speed` m/s along his heading, which the stick (`stick` radians off the camera) sets relative to
 // the camera, as Player does; `drag(t)` gives the counts of a 1280 px wide screen's drag in a frame at time t.
 struct Run {std::vector<float> yaw,rate;float dragged=0,dragYawSum=0;};
 const auto walk=[&](float dt,float seconds,float stick,auto drag,float speed=1.5f){
  Run r;const int frames=int(std::lround(seconds/dt));for(int i=0;i<frames;++i){const float t=i*dt;
   lc.chay=camera.camay+stick;const float c=drag(t,dt);lc.dragAy=dragYaw(c,1280);r.dragged+=c;
   lc.pos.x+=speed*dt*std::sin(lc.chay);lc.pos.z+=speed*dt*std::cos(lc.chay);
   const float before=camera.camay;camera.MoveCamera(dt);if(c!=0)r.dragYawSum+=camera.camay-before;
   r.yaw.push_back(camera.camay);r.rate.push_back((camera.camay-before)/dt);}
  lc.dragAy=0;return r;};
 const auto none=[](float,float){return 0.f;};
 const auto fresh=[&]{camera.camay=0;camera.autoFollow=AutoFollow{};camera.swing.reset();camera.raise.reset();camera.tightRaise=0;lc.pos={};camera.heroPos={};camera.wall=3;camera.openYaw=99;};
 // Following: nothing before kDelay, then it eases in (continuous rate), at most kRate rad/s, towards his heading.
 fresh();{Run r=walk(.016f,AutoFollow::kDelay-.05f,1.2f,none);for(float y:r.yaw)assert(y==0);
  r=walk(.016f,8,1.2f,none);assert(r.yaw.back()>1);float last=0;
  for(float w:r.rate){assert(w>=-1e-4f&&w<=AutoFollow::kRate+1e-4f);assert(std::fabs(w-last)<=2.f*.016f+1e-4f);last=w;}} // no jerk
 // Within kDead of his heading, or straight towards the camera, it does not turn.
 fresh();{Run r=walk(.016f,6,.25f,none);assert(std::fabs(r.yaw.back())<1e-5f);fresh();r=walk(.016f,6,pi,none);assert(std::fabs(r.yaw.back())<1e-4f);}

 // REGRESSION (owner 1665): the drag's turn per screen width is the same before following, while it turns, and after it.
 for(float when:{0.f,3.f}){fresh();
  // A half-width drag (640 counts) in 0.5 s, starting at `when` seconds of walking with a sideways stick.
  Run r=walk(.016f,when+1.f,pi/2,[&](float t,float dt){return t>=when&&t<when+.5f?640*dt/.5f:0.f;});
  assert(r.dragged>600&&std::fabs(r.dragYawSum-dragYaw(r.dragged,1280))<1e-4f);}
 // REGRESSION: after a drag the camera stays where the player put it while he walks on (no swing back, no fight) ...
 fresh();{walk(.016f,3,pi/2,none);const float before=camera.camay;
  Run r=walk(.016f,6,pi/2,[](float t,float){return t<.3f?-20.f:0.f;});
  const float dragged=before+dragYaw(-20*19,1280);for(float y:r.yaw)assert(y<=before+1e-5f);assert(std::fabs(r.yaw.back()-dragged)<.02f);
  // ... and follows again only once he has stood (kStand) and walks on.
  walk(.016f,AutoFollow::kStand+.5f,pi/2,none,0);r=walk(.016f,4,pi/2,none);assert(r.yaw.back()>r.yaw.front()+.3f);}
 // REGRESSION: the same at 30 and 60 fps (and 20): following, drag share and the result.
 {float end[3];int i=0;for(float dt:{1.f/60,1.f/30,1.f/20}){fresh();
   Run r=walk(dt,6,1.f,[&](float t,float d){return t>=2&&t<3?320*d:0.f;});
   assert(std::fabs(r.dragYawSum-dragYaw(r.dragged,1280))<1e-4f);end[i++]=r.yaw.back()-dragYaw(r.dragged,1280);}
assert(std::fabs(end[0]-end[1])<.03f&&std::fabs(end[0]-end[2])<.03f);}
 // REGRESSION (jitter): a steady stick with an uneven hero (every 4th frame no displacement, an uneven frame time) turns
 // smoothly: the rate never jumps, never reverses.
 fresh();{float last=0,t=0;for(int i=0;i<900;++i){const float dt=i%3?1.f/30:1.f/24;t+=dt;lc.chay=camera.camay+1.2f;
   if(i%4)lc.pos.z+=1.5f*dt;const float before=camera.camay;camera.MoveCamera(dt);const float w=(camera.camay-before)/dt;
   assert(w>=-1e-5f&&w<=AutoFollow::kRate+1e-4f&&std::fabs(w-last)<=2.f*dt+1e-4f);last=w;}assert(last>.2f);}

 // Tight spaces. Room enough behind him at the player's pitch: the camera only comes closer.
 const auto settle=[&](int frames){for(int i=0;i<frames;++i)camera.MoveCamera(.016f);};
 fresh();lc.chay=0;camera.wall=1.5f;settle(200);assert(camera.raised==0&&camera.camay==0&&std::fabs(camera.camradius-1.5f)<1e-3f);
 // The wall closer behind, open above only: it rises (critically damped, no overshoot), at most kTightPitch (never top-down),
 // and lets down once there is kTightGain more room than wanted.
 camera.wall=.5f;camera.maxPitch=-9;{float last=0;for(int i=0;i<200;++i){camera.MoveCamera(.016f);assert(camera.raised>=last-1e-6f);last=camera.raised;}}
 assert(camera.raised>.1f&&lc.ax+camera.raised<=kTightPitch+1e-4f&&camera.maxPitch<=kTightPitch+1e-4f&&camera.camay==0);
 camera.wall=1.25f;settle(200);assert(camera.raised>.1f); // just enough at his pitch: stays up (no back and forth)
 camera.wall=3;settle(300);assert(camera.raised==0&&camera.camay==0);
 // REGRESSION (jitter): room flickering around what is wanted, frame by frame, moves neither the pitch nor the yaw back and forth.
 {int flips=0;float last=camera.raised,dir=0;for(int i=0;i<600;++i){camera.wall=i%2?1.1f:1.3f;camera.MoveCamera(.016f);
   const float d=camera.raised-last;if(d*dir<0)++flips;if(d!=0)dir=d;last=camera.raised;assert(camera.camay==0);}assert(flips<=1);}
 camera.wall=3;settle(300);
 // Open to one side: it swings sideways, easing in and out (never faster than kSwingMax), then stays there.
 fresh();camera.wall=.5f;camera.openYaw=-1;{float last=0,lastRate=0;
  for(int i=0;i<300;++i){camera.MoveCamera(.016f);if(!i)assert(camera.autoFollow.held()); // a swing holds following off
   const float w=(last-camera.camay)/.016f;assert(w>=-1e-4f&&w<=kSwingMax+1e-4f&&std::fabs(w-lastRate)<=8.f*.016f);lastRate=w;last=camera.camay;}}
 assert(std::fabs(camera.camay+1)<.01f&&camera.raised==0&&camera.camradius>=kTightClose);
 // A low ceiling: lower before higher; the open side still first.
 fresh();camera.wall=.5f;camera.low=2;camera.ceiling=.4f;settle(200);assert(camera.raised<-.1f&&camera.camay==0);
 // Walled in behind and at the sides, no room above: from in front of him (the cabin).
 fresh();camera.wall=.5f;camera.low=-1;camera.ceiling=.4f;camera.openYaw=pi;settle(600);assert(std::fabs(camera.camay-pi)<.01f&&camera.camradius>=kTightClose);
 // Arriving there (rebirth): the view is taken at once, no swing seen.
 fresh();camera.wall=.5f;camera.ceiling=.4f;camera.openYaw=pi;camera.camradius=3;camera.settle_=true;camera.MoveCamera(.016f);
 assert(std::fabs(camera.camay-pi)<1e-4f&&camera.camradius>=kTightClose);
 // REGRESSION (finger wins): dragged towards the wall, the camera stays there while he walks on; no swing until he has stood and walks again.
 fresh();camera.wall=.5f;camera.ceiling=.4f;camera.openYaw=-1.5f;{
  Run r=walk(.016f,5,0,[](float t,float){return t<.2f?20.f:0.f;},1.5f);const float dragged=dragYaw(20*13,1280);
  for(size_t i=14;i<r.yaw.size();++i)assert(std::fabs(r.yaw[i]-dragged)<1e-4f);
  // A drag stops a swing under way at once.
  camera.autoFollow=AutoFollow{};camera.camay=0;camera.tightAge=0;camera.tightRaise=0;settle(10);assert(camera.swing.value!=0);
  lc.dragAy=.01f;camera.MoveCamera(.016f);lc.dragAy=0;const float held=camera.camay;assert(camera.swing.value==0);settle(200);assert(camera.camay==held);
  lc.dragAx=.01f;camera.MoveCamera(.016f);lc.dragAx=0;} // a vertical drag holds it off too
 camera.ceiling=3;camera.low=-1;fresh();settle(300);
 // tightView alone: the order (closer, side, lower, higher, front), never above kTightPitch, the roomiest when none is good.
 {const auto at=[](float back,float side,float lowR,float high,float front){return [=](float yaw,float pitch){
    const float y=std::fabs(yaw);return y>2?front:y>.2f?side:pitch>.45f?high:pitch<.35f?lowR:back;};};
  TightView v=tightView(.4f,1.2f,true,at(1.3f,0,0,0,0));assert(v.yaw==0&&v.pitch==.4f);
  v=tightView(.4f,1.2f,true,at(.5f,2,0,3,3));assert(v.yaw==.5f&&v.pitch==.4f);
  v=tightView(.4f,1.2f,true,at(.5f,.5f,2,3,3));assert(v.yaw==0&&v.pitch<.4f);
  v=tightView(.4f,1.2f,true,at(.5f,.5f,.5f,3,3));assert(v.yaw==0&&v.pitch>.4f&&v.pitch<=kTightPitch);
  v=tightView(.4f,1.2f,true,at(.5f,.5f,.5f,.5f,2));assert(std::fabs(v.yaw)>2);
  v=tightView(.4f,1.2f,false,at(.5f,2,.5f,.5f,2));assert(v.yaw==0); // no swing just after a drag
  v=tightView(.4f,1.2f,true,at(.5f,.6f,.5f,.8f,.6f));assert(v.yaw==0&&v.pitch>.4f&&v.room==.8f); // none good: the roomiest
  v=tightView(.4f,1.2f,true,at(.5f,.6f,.5f,.6f,.6f));assert(v.yaw==0&&v.pitch==.4f); // not for less than kTightGain
  float highest=0;tightView(.4f,9,true,[&](float,float p){highest=std::fmax(highest,p);return 0.f;});assert(highest<=kTightPitch);
  highest=0;tightView(1.f,9,true,[&](float,float p){highest=std::fmax(highest,p);return 0.f;});assert(highest<=1.f);}
 // Dialogue: the face lands kDialogueFaceY from the top of the screen, above the sheet.
 {const float p22=1.7f,face=.1f,look=dialogueLookElevation(face,p22);const float y=.5f-.5f*std::tan(face-look)*p22;assert(std::fabs(y-kDialogueFaceY)<1e-4f);
  assert(dialogueLookElevation(.3f,0)==.3f);}
 // A fight keeps the native follow camera: it swings behind the fighter and locks his rotation.
 actor.fight=true;lc.chay=1;camera.camay=0;camera.MoveCamera(.016f);assert(camera.camay>0&&actor.locked);
 // Freelook (special mode) keeps the native orbit by dAy.
 actor.fight=false;actor.locked=false;camera.camay=0;lc.chay=0;lc.isSpecialMode=true;lc.dAy=-.1f;camera.MoveCamera(.016f);assert(camera.camay<0);
 const float old=camera.camay;camera.MoveCamera(0);assert(camera.camay==old);
 std::cout<<"PASS: mobile camera: whole drag (half a turn per screen width) the same before/while/after following and at 20/30/60 fps; following eased, no jerk, held off by a drag until the hero stands and walks again; tight spaces: closer, sideways (eased), lower, a little higher (never top-down) or from the front, at once on arrival, no back and forth, never against a drag; dialogue face above the sheet; fight and freelook native\n";
}
'''
with tempfile.TemporaryDirectory(prefix='storm-movement-camera-') as d:
 cpp=Path(d)/'camera.cpp';cpp.write_text(code);exe=Path(d)/'camera'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-D__ANDROID__','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(a.source_root/'src/libs/util/include'),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
