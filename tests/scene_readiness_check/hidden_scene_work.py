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
            subprocess.run(flags + [str(source), '-o', str(exe)], check=True, timeout=12)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=3)
            print(name, 'exit', result.returncode, result.stdout, result.stderr, flush=True)
            if negative:
                assert result.returncode != 0 and 'beauty == 0' in result.stderr, 'negative must fail the beauty assertion'
            else:
                assert result.returncode == 0

        compile_run(ROOT / 'tests/scene_readiness_check/hidden_scene_work.cpp', 'lifecycle')
        draw = (SHIM / 'draw.cpp').read_text()
        branch = block(draw, '    if (hiddenBeauty)')
        # Actual branch must precede probe candidate, primary submit and opaque ack.
        assert draw.index('if (hiddenBeauty)') < draw.index('probe_query = probe.candidate')
        assert draw.index('if (hiddenBeauty)') < draw.index('bgfx::submit(view_id, handle')
        assert draw.index('if (hiddenBeauty)') < draw.index('render::SceneOpaqueDrawSubmitted')
        assert draw.index('fx_lighting.submit(fx_draw)') < draw.index('if (hiddenBeauty)')
        assert draw.index('ObserveSceneTexture') < draw.index('if (hiddenBeauty)')
        base = subprocess.check_output(['git', '-C', str(ROOT), 'show',
            '25686db2872135da1ca8508f87d3143ae6376bf7:src/libs/renderer_bgfx/src/shim/draw.cpp'], text=True)
        old = block(base, '    if (hiddenBeauty)') if '    if (hiddenBeauty)' in base else ''
        fixture = r'''
#include <cassert>
int beauty=0, probe=0, caster=0, discarded=0, invalidated=0;
constexpr int D3D_OK=0, BGFX_DISCARD_ALL=255;
namespace bgfx { void discard(int) {++discarded;} }
struct Device {
 void invalidate_uniforms() {++invalidated;}
 struct Draw {bool caster;} fx_draw;
 struct Lighting {void submit_caster(Device&,int,bool) {++caster; bgfx::discard(BGFX_DISCARD_ALL);}} fx_lighting;
 struct State {int world=0;} s;
 struct Layout {bool skinned=false;} layout_value, *layout=&layout_value;
 int draw(bool hiddenBeauty) {
 BRANCH
 // Continuation is reached only after the real suppression return; GPU calls are not simulated.
 ++beauty; ++probe;
 if(fx_draw.caster) fx_lighting.submit_caster(*this,s.world,layout->skinned);
 return D3D_OK;
 }
};
int main() {
 Device d; d.fx_draw.caster=false; d.draw(true);
 assert(beauty == 0); assert(probe == 0); assert(caster == 0);
 assert(discarded == 1 && invalidated == 1);
 d.fx_draw.caster=true; d.draw(true);
 assert(beauty == 0 && probe == 0 && caster == 1);
 assert(discarded == 2 && invalidated == 2);
 d.fx_draw.caster=false; d.draw(false);
 assert(beauty == 1 && probe == 1 && caster == 1);
}
'''
        for name, snippet, negative in [('before', old, True), ('after', branch, False)]:
            source = tmp / (name + '.cpp')
            source.write_text(fixture.replace('BRANCH', snippet))
            compile_run(source, name, negative)
        device = (SHIM / 'device.cpp').read_text()
        hidden_post = block(device[device.index('void Device::scene_complete()'):],
                            '    if (render::HideSceneBeauty')
        assert 'scene_done = true;' in hidden_post and 'leave_scaled_picture(false)' in hidden_post
        assert 'return;' in hidden_post and 'submit_post' not in hidden_post
        assert device.index(hidden_post) < device.index('const bool probe_placed')
        assert 'views_used > 0 && !render::HideSceneBeauty' in device
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
        print('actual draw suppression branches and post/target-restore source boundaries PASS')


if __name__ == '__main__':
    run()
