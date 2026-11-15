// Read actual producer states without a device or starting workers. This translation unit
// exposes private state only to construct the asynchronous completion ordering under test.
#include <bits/stdc++.h>
#define private public
#include "effects/lamp_shadows.hpp"
#undef private
#include "effects/scene_readiness.hpp"
#include "storm/render/scene_load_action.hpp"
using namespace storm;
int main() {
    effects::LampShadows lamps; lamps.slots_ = 1; lamps.prepared_ = true;
    assert(lamps.initial_state() == 0); // no light/geometry/room resource is needed
    lamps.room_.rgba.resize(4);
    assert(lamps.initial_state() == 2); // room-only upload failed after dirty was cleared
    lamps.room_dirty_ = true; assert(lamps.initial_state() == 1); // upload still due, not failed
    lamps.room_dirty_ = false; lamps.room_map_ = {1}; lamps.active_ = true;
    lamps.static_map_ = {2}; lamps.dynamic_map_ = {3};
    lamps.static_fb_ = {1}; lamps.dynamic_fb_ = {2};
    lamps.layout_dirty_ = lamps.grid_dirty_ = false;
    assert(lamps.initial_state() == 0);
    lamps.grid_.rgba.resize(4); assert(lamps.initial_state() == 2);
    lamps.grid_map_ = {4}; assert(lamps.initial_state() == 0);
    lamps.dynamic_fb_ = BGFX_INVALID_HANDLE; assert(lamps.initial_state() == 2);
    lamps.slots_ = 0; assert(lamps.initial_state() == 0); // disabled resources are not mandatory

    effects::SceneFailures failures;
    assert(!failures.can_retry()); failures.retryable = 1; assert(failures.can_retry());
    failures.reload_required = 1; assert(!failures.can_retry()); // mixed normal + shadow failure
    failures = {}; assert(!failures.can_retry());
    assert(!effects::SceneTextureName("loading/progress.tga"));
    assert(!effects::SceneTextureName("interfaces/button.tga"));
    assert(!effects::SceneTextureName("")); assert(effects::SceneTextureName("characters/sharl1.tga"));
    const auto retry = render::ParseSceneLoadAction("location-load:42\tretry");
    const auto cancel = render::ParseSceneLoadAction("location-load:43\tcancel");
    assert(retry && retry->generation == 42 && !retry->cancel);
    assert(cancel && cancel->generation == 43 && cancel->cancel);
    for (const char *bad : {"location-load:0\tretry", "location-load:-1\tcancel", "location-load:1x\tretry",
          "location-load:18446744073709551616\tretry", "location-load:1\treveal", "location-load:1\tcancel\textra", "other:1\tretry"})
        assert(!render::ParseSceneLoadAction(bad));
    puts("actual producer initial-state chain, failure classes and action wire: PASS");
}
