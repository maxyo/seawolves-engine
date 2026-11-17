#!/usr/bin/env python3
"""Bounded CPU controls; production latch/gate and actual submission branches.

No renderer/GPU execution. Negative uses the original draw branch, not a fake
disabled flag. Temporary compiler outputs are removed on every exit.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHIM = ROOT / 'src/libs/renderer_bgfx/src/shim'


def block(text, marker):
    start = text.index(marker)
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run():
    with tempfile.TemporaryDirectory(prefix='hidden-scene-') as tmp:
        tmp = pathlib.Path(tmp)
        flags = ['c++', '-std=c++20', '-O0', '-g', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer',
                 '-I' + str(ROOT / 'src/libs/renderer_bgfx/include'),
                 '-I' + str(ROOT / 'src/libs/renderer_bgfx/src'),
                 '-I' + str(ROOT / 'src/libs/util/include')]
        def compile_run(source, name, negative=False):
            exe = tmp / name
            # No timeouts: compiles under a loaded land queue take long; the ctest TIMEOUT is the deadlock guard.
            subprocess.run(flags + [str(source), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            print(name, 'exit', result.returncode, result.stdout, result.stderr, flush=True)
            if negative:
                assert result.returncode != 0 and 'beauty == 0' in result.stderr, 'negative must fail the beauty assertion'
            else:
                assert result.returncode == 0

        compile_run(ROOT / 'tests/scene_readiness_check/hidden_scene_work.cpp', 'lifecycle')
        compile_run(ROOT / 'tests/scene_readiness_check/cover_budget.cpp', 'cover-budget')
        draw = (SHIM / 'draw.cpp').read_text()
        branch = block(draw, '    if (hiddenBeauty)')
        fixture = r'''
#include <cassert>
int beauty=0, probe=0, caster=0, discarded=0, invalidated=0;
constexpr int BGFX_DISCARD_ALL=255;
namespace bgfx { void discard(int) {++discarded;} }
struct Device {
 void invalidate_uniforms() {++invalidated;}
 // The colour draw: true when the caster entry (Device::issue -> draw_caster) still offers it to the shadow volumes.
 bool draw(bool hiddenBeauty) {
 BRANCH
 // Continuation is reached only after the real suppression return; GPU calls are not simulated.
 ++beauty; ++probe;
 return true;
 }
 void issue(bool hiddenBeauty) { if (draw(hiddenBeauty)) ++caster; }
};
int main() {
 Device d; d.issue(true);
 assert(beauty == 0); assert(probe == 0);
 assert(caster == 1); // a hidden draw still casts
 assert(discarded == 1 && invalidated == 1);
 d.issue(false);
 assert(beauty == 1 && probe == 1 && caster == 2);
 assert(discarded == 1 && invalidated == 1);
}
'''
        # Negative control: the same draw without the suppression branch must fail.
        for name, snippet, negative in [('without-branch', '', True), ('actual', branch, False)]:
            source = tmp / (name + '.cpp')
            source.write_text(fixture.replace('BRANCH', snippet))
            compile_run(source, name, negative)
        device = (SHIM / 'device.cpp').read_text()
        prefix = device.split('void Device::scene_complete()\n{', 1)[1].split('    auto &host = effects::Host::instance();', 1)[0]
        post_fixture = r'''
#include <cassert>
#include <cstdint>
bool hidden=true;
int copies=0, restored=0, post=0;
namespace render {
 bool HideSceneBeauty(uint64_t,uint64_t) {return hidden;}
}
struct Context {
 struct SurfaceInfo {uint64_t generation=5;} surface;
 static Context& instance() {static Context c; return c;}
 SurfaceInfo& Surface() {return surface;}
};
namespace effects {struct Host {
 static Host& instance() {static Host h; return h;}
 bool post_active() {return true;}
};}
void logf(const char*) {}
struct Store {uint64_t serial=8;};
struct Color {Store* store;};
struct Device {
 bool interface_drawn=false, scene_done=false, scene_drawn=true, interface_first_logged=false;
 Store store; Color color{&store}; Color* back_color=&color; Store* held_color=nullptr;
 void leave_scaled_picture(bool copy) {++restored; copies+=copy; held_color=nullptr;}
 void scene_complete() {
 PREFIX
 ++post;
 }
};
int main() {
 for(bool scaled : {false,true}) {
  Device d; if(scaled) d.held_color=&d.store;
  int r=restored;
  d.scene_complete();
  assert(d.interface_drawn && d.scene_done && !d.held_color);
  assert(restored==r+1 && copies==0 && post==0);
  d.scene_complete(); assert(restored==r+1); // one completion per frame
 }
 hidden=false; Device normal; normal.scene_complete(); assert(post==1);
 Device no_scene; no_scene.scene_drawn=false; no_scene.scene_complete();
 assert(copies==1 && post==1); // no-Fader/non-scene ordinary behavior retained
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>')
        post_source = tmp / 'post.cpp'
        post_source.write_text(post_fixture.replace('PREFIX', prefix))
        compile_run(post_source, 'actual-post-prefix')
        print('actual draw suppression branch and post/target-restore prefix PASS')


if __name__ == '__main__':
    run()
