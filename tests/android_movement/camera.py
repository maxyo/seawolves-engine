#!/usr/bin/env python3
"""Check the real follow camera (mobile camera mode): drag yaw, auto-follow, tight spaces, hero fade, dialogue framing; --source-root supports baseline replay."""
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
 LocationCamera *lc;float camay=0,camradius=3,kRadInert=0,tightRaise=0;Vec3 lookTo,camPos,heroPos;storm::android::AutoFollow autoFollow;
 float wall=3,ceiling=3,openYaw=99,raised=0;
 void MoveCamera(float);
 // Walls `wall` metres behind the hero; raised over him the room is `ceiling`; more towards `openYaw`.
 float FindRadius(float yaw,float pitch){return std::fmax(pitch>lc->ax+.1f?ceiling:wall,3-4*std::fabs(yaw-openYaw));}
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
 // Tight spaces: walls near behind the hero raise the camera over his shoulders, smoothly, then let it down.
 camera.camay=0;lc.chay=0;camera.wall=.5f;for(int i=0;i<200;++i)camera.MoveCamera(.016f);
 assert(camera.raised>.3f&&camera.raised<=kTightPitch-lc.ax+1e-4f&&camera.camradius==3.f&&camera.camay==0);
 camera.wall=3;for(int i=0;i<300;++i)camera.MoveCamera(.016f);assert(camera.raised==0&&camera.camay==0);
 // Cornered (no room above either): it turns calmly towards the side with room, then stays.
 camera.wall=.4f;camera.ceiling=.5f;camera.openYaw=kCorneredTurns[1];float last=0;
 for(int i=0;i<200;++i){camera.MoveCamera(.016f);assert(camera.camay<=last&&last-camera.camay<=kCorneredRate*.016f+1e-6f);last=camera.camay;}
 assert(camera.camay<-.05f&&camera.FindRadius(camera.camay,lc.ax)>=kCorneredRoom-.05f);
 {const float none[4]={.5f,.5f,.6f,.4f},right[4]={.5f,2,1,1};assert(corneredTurn(.5f,none,.1f)==0&&corneredTurn(.5f,right,.1f)==-kCorneredRate*.1f);}
 camera.wall=3;camera.ceiling=3;camera.openYaw=99;camera.camay=0;for(int i=0;i<300;++i)camera.MoveCamera(.016f);
 // The pitch with the most room, a higher one only for a clear gain; never below the player's own.
 {const float a[3]={1,1.2f,1.25f},b[3]={.4f,.8f,2},c[3]={.4f,2,2.2f},d[3]={2,.3f,.3f};
  assert(tightPitch(.4f,a)==.4f&&tightPitch(.4f,b)==kTightPitch&&std::fabs(tightPitch(.4f,c)-tightPitchAt(.4f,1))<1e-6f&&tightPitch(.4f,d)==.4f&&tightPitch(1.4f,b)==1.4f);}
 // Dialogue: the face lands kDialogueFaceY from the top of the screen, above the sheet.
 {const float p22=1.7f,face=.1f,look=dialogueLookElevation(face,p22);const float y=.5f-.5f*std::tan(face-look)*p22;assert(std::fabs(y-kDialogueFaceY)<1e-4f);
  assert(dialogueLookElevation(.3f,0)==.3f);}
 // A fight keeps the native follow camera: it swings behind the fighter and locks his rotation.
 actor.fight=true;lc.chay=1;camera.camay=0;camera.MoveCamera(.016f);assert(camera.camay>0&&actor.locked);
 // Freelook (special mode) keeps the native orbit by dAy.
 actor.fight=false;actor.locked=false;camera.camay=0;lc.chay=0;lc.isSpecialMode=true;lc.dAy=-.1f;camera.MoveCamera(.016f);assert(camera.camay<0);
 const float old=camera.camay;camera.MoveCamera(0);assert(camera.camay==old);
 std::cout<<"PASS: mobile camera: whole drag (half a turn per screen width), auto-follow behind the moving hero after a delay (paused by a drag), tight spaces raise it or turn it out of a corner, dialogue face above the sheet; fight and freelook native\n";
}
'''
with tempfile.TemporaryDirectory(prefix='storm-movement-camera-') as d:
 cpp=Path(d)/'camera.cpp';cpp.write_text(code);exe=Path(d)/'camera'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-D__ANDROID__','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(a.source_root/'src/libs/util/include'),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
