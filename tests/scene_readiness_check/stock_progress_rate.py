#!/usr/bin/env python3
"""Production ProgressView/DrawProgressView on a clock: a load that creates many resources between progress frames
(wdmCreateWorldMap: every texture, geometry file and buffer calls ProgressView) spends at most a fifth of its time
presenting them, also when a present takes longer than the stock 50 ms (a 15 Hz display)."""
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
methods = '\n'.join(function(renderer, 'void Renderer::'+name) for name in ['ProgressView()', 'DrawProgressView(bool present)'])
source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "loading_transaction.hpp"
uint32_t now = 1000, present_ms = 0, presents = 0, presenting = 0;
uint32_t SDL_GetTicks(){return now;}
namespace glm { struct vec3 { float x, y, z; vec3(float a, float b, float c) : x(a), y(b), z(c) {} }; }
namespace storm::render { void ArmSceneOpaqueDraw(bool, bool, int){} }
struct D3DVIEWPORT9 { uint32_t X = 0, Y = 0, Width = 1280, Height = 720; };
enum { D3DPT_TRIANGLESTRIP = 5, D3DFVF_XYZRHW = 4, D3DFVF_DIFFUSE = 0x40, D3DFVF_TEX1 = 0x100 };
struct Device { void Present(){now += present_ms; presenting += present_ms; ++presents;} };
struct Renderer {
 static constexpr uint32_t kProgressPresentGapMs = 50;
 uint32_t progressPresentGap = kProgressPresentGapMs, progressPresents = 0, progressPresentMs = 0, progressUpdateTime = 0;
 int progressTexture = 1, backTexture = -1, back0Texture = 2, progressTipsTexture = -1;
 int progressSafeCounter = 0, loadFrame = 0, progressFramesCountX = 8, progressFramesCountY = 8;
 float progressFramesPosX = 0.85f, progressFramesPosY = 0.8f, progressFramesWidth = 0.0625f, progressFramesHeight = 0.0625f;
 float m_fHeightDeformator = 1.0f;
 bool isInPViewProcess = false;
 Device device; Device *d3d9 = &device;
 void BeginScene(){} void EndScene(){} void TextureSet(int, int){}
 void GetViewport(D3DVIEWPORT9 *vp){*vp = {};}
 void DrawPrimitiveUP(int, int, int, const void *, int, const char *){}
 void ProgressView(); void DrawProgressView(bool present);
};
METHODS
// A load of `resources` creations, `work` ms each, each calling ProgressView, on a display that takes `present` ms.
uint32_t load(uint32_t resources, uint32_t work, uint32_t present)
{
 Renderer r; present_ms = present; presents = presenting = 0; now = 1000;
 r.progressUpdateTime = now - 1000; // StartProgressView: the first progress frame at once
 const uint32_t start = now;
 for (uint32_t i = 0; i < resources; ++i) { r.ProgressView(); now += work; }
 const uint32_t total = now - start;
 std::fprintf(stderr, "%u resources of %u ms, present %u ms: %u ms, %u frames, %u ms presenting\n", resources, work, present,
             total, presents, presenting);
 assert(presents >= 1);
 assert(presenting * 5 <= total + 5 * present); // at most a fifth of the load, give or take the last frame
 return total;
}
int main(){
 // The emulator's first sea-to-map trip: ~450 creations, a 66 ms present. The stock throttle (50 ms from one
 // present's start to the next) presented at every creation: 30 s instead of the ~1 s of work.
 assert(load(450, 2, 66) <= 900 * 5 / 4 + 66);
 assert(load(450, 2, 16) <= 900 * 5 / 4 + 16);
 load(40, 30, 33);
 // The stock rate stays the floor: a fast display still shows a progress frame every 50 ms of work at most.
 load(100, 10, 1); assert(presents >= 15);
 // The loading transaction draws the screen itself: ProgressView presents nothing then.
 Renderer r; storm::loading::active = true; presents = 0; r.ProgressView(); assert(presents == 0);
}
'''.replace('METHODS', methods)
with tempfile.TemporaryDirectory(prefix='stock-progress-rate-check-') as tmp:
    p = Path(tmp); (p/'check.cpp').write_text(source)
    subprocess.run(['c++', '-std=c++17', '-O0', '-fsanitize=address,undefined', '-I'+str(root/'src/libs/util/include'),
                    str(p/'check.cpp'), '-o', str(p/'check')], check=True)
    subprocess.run([str(p/'check')], check=True)
print('Loading screen progress frames between pieces of loading work PASS')
