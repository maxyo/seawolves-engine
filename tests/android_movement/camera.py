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
struct Vec3 {float x=0,y=0,z=0;Vec3 operator-(const Vec3&o)const{return {x-o.x,y-o.y,z-o.z};}};
namespace glm{using vec3=Vec3;}
struct Character {bool fight=false,locked=false;bool MobileCameraOrbit()const{return !fight;}void LockRotate(bool value){locked=value;}};
struct LocationCamera {float chay=0,dAy=0,dragAy=0,lheight=1,rotInertia=8,ax=.4f,radius=3;bool isSpecialMode=false;Vec3 pos;Character *character;};
struct CameraFollow {
 LocationCamera *lc;float camay=0,camradius=3,kRadInert=0,tightRaise=0,tightPitch=0,swingLeft=0,tightAge=0,dragHold=0;bool settle_=false;
 Vec3 lookTo,camPos,heroPos;storm::android::AutoFollow autoFollow;
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
 // Moving, the camera eases round behind the hero's heading after the delay, never before it.
 camera.camay=0;lc.chay=1.2f;
 const auto walk=[&](float dt){lc.pos.z+=1.5f*dt;camera.MoveCamera(dt);}; // the hero walks at 1.5 m/s
 float t=0;for(;t<AutoFollow::kDelay-.05f;t+=.016f)walk(.016f);assert(camera.camay==0);
 for(int i=0;i<60*8;++i){const float before=camera.camay;walk(.016f);assert(camera.camay>=before&&camera.camay<=lc.chay+1e-4f);
   assert(camera.camay-before<=AutoFollow::kRate*.016f+1e-6f);} // calm: at most kRate rad/s
 assert(std::fabs(camera.camay-lc.chay)<.05f);
 // The same over the frame rates (60 fps against 10 fps).
 {AutoFollow fast,slow;float a=0,b=0;for(int i=0;i<180;++i)a+=fast.step(1.f/60,false,true,1.f-a);for(int i=0;i<30;++i)b+=slow.step(.1f,false,true,1.f-b);assert(std::fabs(a-b)<.03f);}
 // Straight towards the camera it does not swing (the stick steers relative to the camera).
 {AutoFollow f;for(int i=0;i<600;++i)assert(std::fabs(f.step(.016f,false,true,pi))<1e-6f);}
 // A drag stops it and pauses it until the hero stands and moves again.
 {AutoFollow f;for(int i=0;i<100;++i)f.step(.016f,false,true,1);assert(f.step(.016f,false,true,1)>0);
  assert(f.step(.016f,true,true,1)==0&&f.paused());for(int i=0;i<300;++i)assert(f.step(.016f,false,true,1)==0);
  f.step(.016f,false,false,1);assert(!f.paused());float turn=0;for(int i=0;i<200;++i)turn+=f.step(.016f,false,true,1);assert(turn>0);}
 // Tight spaces. Room enough behind him at the player's pitch: the camera only comes closer.
 const auto settle=[&](int frames){for(int i=0;i<frames;++i)camera.MoveCamera(.016f);};
 camera.camay=0;lc.chay=0;camera.wall=1.5f;settle(200);assert(camera.raised==0&&camera.camay==0&&std::fabs(camera.camradius-1.5f)<1e-3f);
 // The wall closer behind, open above only: it rises a little, at most kTightPitch (never top-down), then lets down.
 camera.wall=.5f;camera.maxPitch=-9;settle(200);
 assert(camera.raised>.1f&&lc.ax+camera.raised<=kTightPitch+1e-4f&&camera.maxPitch<=kTightPitch+1e-4f&&camera.camay==0);
 camera.wall=3;settle(300);assert(camera.raised==0&&camera.camay==0);
 // Open to one side: it swings sideways (calmly) rather than rising, then stays there.
 camera.wall=.5f;camera.openYaw=-1;float last=0;
 for(int i=0;i<200;++i){camera.MoveCamera(.016f);assert(camera.camay<=last+1e-6f&&last-camera.camay<=kSwingRate*.016f+1e-5f);last=camera.camay;}
 assert(std::fabs(camera.camay+1)<.01f&&camera.raised==0&&camera.camradius>=kTightClose);
 // A low ceiling: lower before higher; the open side still first.
 camera.camay=0;camera.openYaw=99;camera.low=2;camera.ceiling=.4f;settle(200);assert(camera.raised<-.1f&&camera.camay==0);
 // Walled in behind and at the sides, no room above: from in front of him (the cabin).
 camera.low=-1;camera.openYaw=3.14159265f;settle(400);assert(std::fabs(camera.camay-3.14159265f)<.01f&&camera.camradius>=kTightClose);
 // Arriving there (rebirth): the view is taken at once, no swing seen.
 camera.camay=0;camera.camradius=3;camera.tightRaise=0;camera.settle_=true;camera.MoveCamera(.016f);
 assert(std::fabs(camera.camay-3.14159265f)<1e-4f&&camera.camradius>=kTightClose);
 // After a drag it does not swing for kDragHold seconds (the player's own view).
 camera.camay=0;lc.dragAy=.01f;camera.MoveCamera(.016f);lc.dragAy=0;const float dragged=camera.camay;
 for(float t=.016f;t<kDragHold-.05f;t+=.016f){camera.MoveCamera(.016f);assert(camera.camay==dragged);}
 settle(200);assert(std::fabs(camera.camay-3.14159265f)<.01f);
 camera.wall=3;camera.ceiling=3;camera.openYaw=99;camera.camay=0;settle(300);
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
 std::cout<<"PASS: mobile camera: whole drag (half a turn per screen width), auto-follow behind the moving hero after a delay (paused by a drag), tight spaces: closer, sideways, lower, a little higher (never top-down) or from the front, at once on arrival, dialogue face above the sheet; fight and freelook native\n";
}
'''
with tempfile.TemporaryDirectory(prefix='storm-movement-camera-') as d:
 cpp=Path(d)/'camera.cpp';cpp.write_text(code);exe=Path(d)/'camera'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-D__ANDROID__','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(a.source_root/'src/libs/util/include'),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
