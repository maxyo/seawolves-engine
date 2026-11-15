// Read actual producer states without a device or starting workers. This translation unit
// exposes private state only to construct the asynchronous completion ordering under test.
#include <bits/stdc++.h>
#define private public
#include "effects/town_light.hpp"
#include "effects/ao_volume.hpp"
#include "effects/lamp_shadows.hpp"
#undef private
#include "effects/scene_readiness.hpp"
#include "storm/render/scene_load_action.hpp"
using namespace storm;
int main() {
    effects::TownLight town;
    town.enabled_ = true; town.positions_.resize(3); town.lightmaps_.resize(1);
    assert(town.initial_state() == 1); // registration is not the first completed bake
    town.place_prepared_ = true;
    assert(town.initial_state() == 0); // all calibrated maps rejected: genuinely inapplicable
    town.maps_.resize(1); town.maps_[0].texture = {1}; town.ready_ = true;
    town.bounce_scene_ = std::make_shared<effects::Bounce::Scene>();
    assert(town.initial_state() == 1); // first sun map lacks the scene's bounce
    town.sun_job_.job_ = std::make_shared<effects::TownLight::SunJob>();
    town.sun_bounce_ = std::make_shared<fx::BounceField>();
    assert(town.initial_state() == 1); // ready_ true cannot hide corrective bake in flight
    town.sun_job_.job_.reset(); town.blend_ = 0.5f;
    assert(town.initial_state() == 1); // upload/blend is part of initial completion
    town.blend_ = 1.0f; assert(town.initial_state() == 0);
    town.maps_[0].texture = BGFX_INVALID_HANDLE; assert(town.initial_state() == 2);
    town.maps_[0].texture = {1};
    town.wall_job_.job_ = std::make_shared<effects::TownLight::WallJob>();
    town.wall_job_->state = 2; assert(town.initial_state() == 2);

    effects::Bounce bounce; bounce.enabled_ = true; bounce.indices_.resize(3);
    using V = effects::Visibility::State;
    assert(bounce.initial_state(V::Building) == 1);
    assert(bounce.initial_state(V::Failed) == 2);
    bounce.unavailable_ = true; assert(bounce.initial_state(V::Ready) == 2); // oversized stacked volume
    bounce.unavailable_ = false; assert(bounce.initial_state(V::Ready) == 1);
    bounce.scene_ = std::make_shared<effects::Bounce::Scene>();
    bounce.sun_field_ = std::make_shared<fx::BounceField>();
    assert(bounce.initial_state(V::Ready) == 1); // lamps still in flight
    bounce.lamp_field_ = std::make_shared<fx::BounceField>();
    assert(bounce.initial_state(V::Ready) == 0);
    bounce.failed_ = true; assert(bounce.initial_state(V::Ready) == 2);
    effects::AoVolumes ao; ao.positions_.resize(3);
    assert(ao.initial_state(true) == 1);
    ao.job_.job_ = std::make_shared<effects::AoVolumes::Job>();
    ao.job_->state = effects::AoVolumes::Job::Failed;
    assert(ao.initial_state(true) == 2); assert(ao.initial_state(false) == 0);

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
