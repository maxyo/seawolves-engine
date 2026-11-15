#!/usr/bin/env python3
"""Actual stock draw snippets: no loading RNG/emission/simulation. CPU-only.
This is a focused side-effect control, not a substitute for target compilation or
real sea/cabin/boarding readiness and frame evidence.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]

def method(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

rain = (root / 'src/libs/weather/src/rain.cpp').read_text()
render = method(rain, 'void RAIN::Realize(')
start = render.index('                pRainBlocks[i].vPos.y -=')
if 'if (!storm::loading::active)' in render[:start]:
    start = render.rindex('                if (!storm::loading::active)', 0, start)
end = render.index('                rs->SetTransform', start)
rain_block = render[start:end]
drops = method(rain, 'void RAIN::RealizeDrops(')
start = drops.index('    int32_t iNumNewDrops1 = 0, iNumNewDrops2 = 0;') if 'int32_t iNumNewDrops1 = 0' in drops else drops.index('    fDropsDeltaTime +=')
end = drops.index('    if (auto && entities', start)
accumulator = drops[start:end]
manager = method((root / 'src/libs/particles/src/manager/particle_manager.cpp').read_text(), 'void ParticleManager::DrawCurrent()')
wrapper = method((root / 'src/libs/particles/src/k2_wrapper/particles.cpp').read_text(), 'void PARTICLES::LoadingRealize()')
tracks = method((root / 'src/libs/ship/src/track.cpp').read_text(), 'void ShipTracks::Realize(')
source = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include "loading_transaction.hpp"
// Exact constants from math_inlines.h; no math helper behavior is replaced.
constexpr float PI=3.14159265358979323846f;
constexpr float PIm2=PI*2.0f;
int rng=0,respawn=0;
float FRAND(float x){++rng;return x*.75f;}
struct Vec{float x=0,y=0,z=0;};
struct Block{Vec vPos,vAng;float fWindFlaw=0;uint32_t dwTime=0;} blocks[1];
Block *pRainBlocks=blocks;
Vec vCamPos{0,100,0};
float fRainSpeed=10,fRainJitter=1,fRainWindSpeed=2,fWindAngle=.5f,fRainHeight=5;
void InitialSomeBlockParameters(unsigned){++respawn;}
void drawRain(uint32_t Delta_Time){unsigned i=0; RAIN_BLOCK }
float fDropsDeltaTime=.99f;unsigned dwDropsNearNum=5,dwDropsFarNum=3;
int dropCount(float fDeltaTime){ ACCUMULATOR return iNumNewDrops1+iNumNewDrops2; }
struct RS {int binds=0;void TextureSet(int,int){++binds;}} rs;
struct Processor{int draws=0;void Draw(){++draws;}} bb,mdl;
struct ParticleManager{RS *pRS=&rs;Processor *BB_Processor=&bb,*MDL_Processor=&mdl;
 int pProjectTexture=1,pProjectTextureNormalMap=2;void DrawCurrent();} manager;
struct PARTICLES{ParticleManager*pManager=&manager;void LoadingRealize();};
struct Track{int executes=0,prepares=0,draws=0;
 void Execute(float){++executes;}void PrepareVertices(){++prepares;}void Realize(float){++draws;}} track;
struct ShipTracks{std::vector<Track*> aShips{&track};void Realize(uint32_t);};
MANAGER
WRAPPER
TRACKS
int main(){
 storm::loading::active=true;
 const Block before=blocks[0];
 for(int i=0;i<20;++i){drawRain(0);assert(dropCount(0)==0);}
 assert(rng==0&&respawn==0&&fDropsDeltaTime==.99f);
 assert(blocks[0].vPos.y==before.vPos.y&&blocks[0].vAng.y==before.vAng.y);
 assert(blocks[0].fWindFlaw==before.fWindFlaw&&blocks[0].dwTime==before.dwTime);
 PARTICLES particles;ShipTracks shipTracks;
 for(int i=0;i<20;++i){particles.LoadingRealize();shipTracks.Realize(0);}
 assert(rs.binds==60&&bb.draws==20&&mdl.draws==20);
 assert(track.executes==0&&track.prepares==20&&track.draws==20);
 particles.pManager=nullptr;particles.LoadingRealize();
 storm::loading::active=false;
 drawRain(0);assert(rng==2&&respawn==1);assert(dropCount(0)>0);
 shipTracks.Realize(16);assert(track.executes==1&&track.prepares==20&&track.draws==21);
}
'''
for token, value in [('RAIN_BLOCK', rain_block), ('ACCUMULATOR', accumulator), ('MANAGER', manager), ('WRAPPER', wrapper), ('TRACKS', tracks)]:
    source = source.replace(token, value)
with tempfile.TemporaryDirectory(prefix='stock-consumers-') as tmp:
    path=Path(tmp)
    (path/'check.cpp').write_text(source)
    subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(root/'src/libs/util/include'),str(path/'check.cpp'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True)
print('actual stock rain/particles/track loading side-effect control PASS')
