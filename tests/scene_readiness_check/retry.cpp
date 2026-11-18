// Real worker/cache retry, Noop submission only: no model inference and no GPU pixel claim.
// Waits are on the worker's progress, never on a clock: a step that cannot finish hangs and tests/renderer_bgfx/run.py's
// deadlock guard reports the last "step:" line.
#include <bits/stdc++.h>
#include "effects/normal_maps.hpp"
#define private public
#include "effects/upscale.hpp"
#include "effects/shadows.hpp"
#undef private
#include "effects/relief_generator.hpp"
#include "shim/shim.hpp"
#include "storm/fx/lighting.hpp"
#include "storm/fx/relief_store.hpp"
#include "texture/core/service.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <unistd.h>
using namespace storm;
int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("storm_scene_retry_" + std::to_string(getpid()));
    effects::ConfigureRelief({dir, dir / "sources", nullptr});
    effects::NormalMaps maps; maps.configure(true,512,false,true);
    fx::Lighting lighting;
    effects::NormalMaps gloss_only; gloss_only.configure(false,512,true,true); gloss_only.configure_gloss(true,512);
    shim::TexStore gloss_store; gloss_store.normal_map = std::make_shared<effects::NormalMapEntry>();
    gloss_store.normal_map->relief = fx::ReliefClass::Stone;
    gloss_store.normal_map->state = effects::NormalMapEntry::Waiting;
    gloss_store.normal_map->material_gloss = fx::GlossClass::Stone;
    gloss_store.normal_map->drawn = 1; // a draw of this scene read it
    bool required_inference = false;
    assert(gloss_only.prepare_scene(gloss_store,lighting,&required_inference) == 1 && required_inference); // actual gloss-only consumer
    gloss_store.normal_map->material_gloss = fx::GlossClass::None;
    assert(gloss_only.prepare_scene(gloss_store,lighting,&required_inference) == 0 && !required_inference); // disabled normal not a thermal wait
    puts("gloss-only normal-map material accounting: PASS");
    // The generator takes the products of the last configured NormalMaps (the renderer has one): relief on again.
    maps.configure(true,512,false,true);
    shim::TexStore store;
    store.normal_map = std::make_shared<effects::NormalMapEntry>();
    auto &entry = *store.normal_map;
    entry.name="scene_retry_stone"; entry.relief=fx::ReliefClass::Stone; entry.key=0x5343454e45;
    entry.state=effects::NormalMapEntry::Unqueued;
    shim::TexStore undrawn; undrawn.normal_map = std::make_shared<effects::NormalMapEntry>();
    undrawn.normal_map->relief=fx::ReliefClass::Stone; undrawn.normal_map->key=0x554e4452;
    assert(maps.prepare_scene(undrawn,lighting)==0 && undrawn.normal_map->state==effects::NormalMapEntry::Unqueued);
    puts("a scene texture no draw read is not applicable and starts nothing: PASS");
    entry.drawn=1; // a draw of this scene read it
    puts("step: the unavailable generator reports the entry Missing"); fflush(stdout);
    while (maps.prepare_scene(store,lighting) != 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(entry.state == effects::NormalMapEntry::Missing); // actual unavailable generator result
    assert(maps.prepare_scene(store,lighting)==2);
    fx::NormalMap map{8,8,4,fx::NormalMapFormat::Rg8,
        std::vector<uint8_t>(fx::NormalMapBytes(8,8,fx::NormalMapFormat::Rg8),128)};
    assert(fx::WriteReliefMap(dir,entry.key,map));
    assert(maps.prepare_scene(store,lighting)==2); // file alone does not clear stale producer failure
    if (!std::getenv("STORM_SCENE_SKIP_PRODUCER_RETRY")) maps.retry_scene(store);
    if (entry.state != effects::NormalMapEntry::Unqueued) {
        effects::ReliefGenerator::instance().shutdown(); texture::Service::instance().shutdown();
        std::filesystem::remove_all(dir);
        puts("FAIL: cache presence plus UI retry without producer reset leaves Missing"); return 1;
    }
    bgfx::Init init; init.type=bgfx::RendererType::Noop; init.swapChain.width=16;init.swapChain.height=16;
    assert(bgfx::init(init));
    effects::SunShadows sun; sun.settings_.shadows = fx::ShadowQuality::Soft;
    sun.sun_present_ = sun.prepared_ = sun.active_ = true;
    assert(sun.initial_state() == 0); // no still casters
    sun.caster_indices_.resize(3); sun.tiles_dirty_ = true; assert(sun.initial_state() == 1);
    sun.tiles_dirty_ = false; assert(sun.initial_state() == 2); // required VB failed
    sun.tiles_vb_ = {1}; sun.tiles_failed_ = true; assert(sun.initial_state() == 2); // required solid/cut IB failed
    sun.tiles_failed_ = false; assert(sun.initial_state() == 0); // cut-only tile needs no solid IB
    sun.tiles_vb_ = BGFX_INVALID_HANDLE;
    puts("step: the retried entry is read, uploaded and faded in"); fflush(stdout);
    int result=1;
    while ((result=maps.prepare_scene(store,lighting))==1) {
        maps.end_frame(); bgfx::frame(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(result==0 && entry.state==effects::NormalMapEntry::Uploaded && bgfx::isValid(entry.handle));
    // All-location generated data is distinct from the budget-selected residency set.
    effects::Upscales x2; x2.wanted_=x2.enabled_=x2.configured_=x2.supported_=true;
    store.upscale=std::make_shared<effects::UpscaleEntry>(); auto &up=*store.upscale;
    up.state=effects::UpscaleEntry::Stored;
    assert(x2.prepare_scene(store)==1); // no plan yet: cannot claim a residency set
    x2.planned_=true; assert(x2.prepare_scene(store)==0); // off-camera data generated, no upload budget
    up.state=effects::UpscaleEntry::Waiting; assert(x2.prepare_scene(store)==1); // queue empty while worker owns it
    up.state=effects::UpscaleEntry::Stored; up.selected=true; assert(x2.prepare_scene(store)==1);
    up.state=effects::UpscaleEntry::Read; assert(x2.prepare_scene(store)==1); // CPU data is not resident
    up.state=effects::UpscaleEntry::Resident; assert(x2.prepare_scene(store)==1); // invalid handle
    up.handle=entry.handle; assert(x2.prepare_scene(store)==0); up.handle=BGFX_INVALID_HANDLE;
    up.state=effects::UpscaleEntry::Failed; assert(x2.prepare_scene(store)==2);
    x2.supported_=false; store.upscale.reset(); assert(x2.prepare_scene(store)==3); // omitted entry != supported
    // Retry must not requeue a completed or currently owned worker entry.
    maps.retry_scene(store); assert(entry.state==effects::NormalMapEntry::Uploaded);
    bgfx::destroy(entry.handle);entry.handle=BGFX_INVALID_HANDLE; bgfx::frame();bgfx::shutdown();
    effects::ReliefGenerator::instance().shutdown(); texture::Service::instance().shutdown();
    std::filesystem::remove_all(dir);
    puts("x2 generated/resident/budget and unsupported-state controls: PASS");
    puts("actual Missing -> unchanged despite cache -> Retry -> read/upload/fade complete: PASS (Noop)");
}
