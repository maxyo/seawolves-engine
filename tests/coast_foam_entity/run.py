#!/usr/bin/env python3
"""Compile production foam sea lookup and height-field upload against a destroy/recreate registry."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-ref', help='read production source at a baseline git ref')
a = p.parse_args()
def read(path):
    return subprocess.check_output(['git', '-C', str(root), 'show', f'{a.source_ref}:{path}'], text=True) if a.source_ref else (root/path).read_text()
source = read('src/libs/island/src/foam.cpp')
realize = source[source.index('void CoastFoam::Realize('):]
lookup = realize[realize.index('    pFrustumPlanes = rs->GetPlanes();') + len('    pFrustumPlanes = rs->GetPlanes();'):realize.index('    auto fDeltaTime')]
heights = source[source.index('bool CoastFoam::UpdateHeights('):]
heights = heights[:heights.index('\n}\n') + 3].replace('CoastFoam::', '')
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "FOAM_GPU"
namespace glm { struct vec3 { float x=0, y=0, z=0; }; }
using entid_t = int;
struct D3DLOCKED_BOX { int RowPitch=0, SlicePitch=0; void *pBits=nullptr; };
enum { D3DFMT_A8R8G8B8 = 21, D3DPOOL_MANAGED = 1 };
struct SeaHeightField {
    const uint8_t *const *frames = nullptr; int32_t frameCount = 0, size = 0;
    float frame[2]{}, scale[2]{}, amplitude[2]{}; glm::vec3 move[2]{}; glm::vec3 camera{}; float reach = 0;
};
struct SEA_BASE {
    std::vector<std::vector<uint8_t>> data; std::vector<const uint8_t *> pointers;
    explicit SEA_BASE(uint8_t value) : data(foam_gpu::kFrames, std::vector<uint8_t>(foam_gpu::kSize * foam_gpu::kSize, value)) {
        for (auto &frame : data) pointers.push_back(frame.data());
    }
    bool HeightField(SeaHeightField &f) const {
        f.frames = pointers.data(); f.frameCount = foam_gpu::kFrames; f.size = foam_gpu::kSize; return true;
    }
};
struct Core {
    SEA_BASE *active = nullptr; int serial = 0;
    entid_t GetEntityId(const char *name) { assert(std::string(name)=="sea"); return active ? serial : 0; }
    SEA_BASE *GetEntityPointerSafe(entid_t id) { return id ? active : nullptr; }
    void Trace(const char *, ...) {}
} core;
struct Renderer {
    std::vector<uint32_t> volume; int uploads = 0, created = 0;
    void *GetPlanes() { return nullptr; }
    int VolumeTextureCreate(int w, int h, int d, int, int, int, int) { volume.assign(size_t(w) * h * d, 0); return created++; }
    bool TextureLockBox(int, int, D3DLOCKED_BOX &box, int) {
        box.pBits = volume.data(); box.RowPitch = foam_gpu::kSize * 4; box.SlicePitch = box.RowPitch * foam_gpu::kSize; return true;
    }
    void TextureUnlockBox(int, int) { uploads++; }
} renderer, *rs = &renderer;
struct CoastFoam {
    void *pFrustumPlanes = nullptr;
    int32_t iHeightTexture = -1; entid_t heightSea{}; const uint8_t *const *pHeightFrames = nullptr; bool bHeightsLogged = false;
    bool drawn = false;
    HEIGHTS
    void Realize(uint32_t) {
        drawn = false;
        LOOKUP
        SeaHeightField field;
        drawn = UpdateHeights(seaId, Sea, field);
    }
};
int main() {
    CoastFoam foam;
    foam.Realize(16); assert(!foam.drawn && renderer.uploads == 0);
    auto sea = std::make_unique<SEA_BASE>(3); core.active = sea.get(); core.serial = 1;
    foam.Realize(16); assert(foam.drawn && renderer.uploads == 1 && renderer.volume[0] == 0x03030303u);
    foam.Realize(16); assert(foam.drawn && renderer.uploads == 1); // once per sea
    sea.reset(); core.active = nullptr;
    foam.Realize(16); assert(!foam.drawn && renderer.uploads == 1);
    sea = std::make_unique<SEA_BASE>(7); core.active = sea.get(); core.serial = 2;
    foam.Realize(16); assert(foam.drawn && renderer.uploads == 2 && renderer.volume[0] == 0x07070707u && renderer.created == 1);
    puts("PASS production CoastFoam sea lookup and height upload: absent/destroyed/recreated sea");
}
'''
code = code.replace('FOAM_GPU', str(root / 'src/libs/island/src/foam_gpu.hpp')).replace('HEIGHTS', heights).replace('LOOKUP', lookup)
with tempfile.TemporaryDirectory(prefix='coast-foam-entity-') as d:
    out=Path(d); (out/'check.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-fno-omit-frame-pointer',str(out/'check.cpp'),'-o',str(out/'check')],check=True)
    subprocess.run([str(out/'check')],check=True)
