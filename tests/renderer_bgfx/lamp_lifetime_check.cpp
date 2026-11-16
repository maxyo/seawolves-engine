// CPU lifetime regression: the game releases texture wrappers after reporting geometry, while lamp caches still
// need their alpha textures. No window/GPU: Noop only supplies buffer handles for reported ship geometry.
#include "effects/lamp_shadows.hpp"
#include "effects/lighting.hpp"
#include "effects/shadows.hpp"
#include "shim/device.hpp"
#include "shim/shim.hpp"
#include "storm/fx/cutout.hpp"
#include "../../src/libs/location/src/light_components.hpp"

#include <cstdio>
#include <memory>

using namespace storm;

namespace
{
int failures = 0;
void expect(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", what);
    failures += !pass;
}
shim::Texture2D *make_cutout_texture()
{
    auto store = shim::make_store(nullptr, shim::TexKind::Tex2D, 2, 2, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    store->cutout = std::make_shared<fx::AlphaMask>();
    return new shim::Texture2D(std::move(store));
}
void aggregate_transport(shim::Device &device)
{
    auto snapshot=std::make_shared<fx::AggregateSnapshot>();
    snapshot->members.push_back({10,{1,2,3},{2.2f,1.5f,.5f},{2.2f,1.5f,.5f},15,{.5f,0,1.5f}});
    snapshot->groups.push_back({10,{1,2,3},0,1,15,3.6f,{.5f,0,1.5f},{3.6f,.25f,.4f},false});
    D3DLIGHT9 light{};light.Type=D3DLIGHT_POINT;
    expect(device.SetLight(5,&light)==D3D_OK,"raw point accepted before indexed aggregate metadata");
    render::SetEffectsLightAggregate(5,{snapshot,0});
    expect(device.lights[5].aggregate.snapshot==snapshot,"successful indexed bridge keeps exact immutable snapshot");
    device.LightEnable(5,false);
    expect(device.lights[5].aggregate.snapshot==snapshot,"LightEnable preserves aggregate identity");
    shim::StateBlock state(&device);
    auto newer=std::make_shared<fx::AggregateSnapshot>(*snapshot);newer->members[0].color={.1f,.2f,.3f};
    device.SetLight(5,&light);render::SetEffectsLightAggregate(5,{newer,0});
    expect(device.lights[5].aggregate.snapshot==newer,"new frame binds new snapshot without mutating old one");
    state.Apply();
    expect(device.lights[5].aggregate.snapshot==snapshot && device.lights[5].aggregate.snapshot->members[0].color[0]==2.2f,
           "stateblock restores old immutable RGB and exact group index after a newer frame");
    device.SetLight(5,&light);
    expect(!device.lights[5].aggregate.get(),"plain SetLight clears old aggregate identity");
    render::SetEffectsLightAggregate(5,{snapshot,0});
    expect(device.SetLight(5,nullptr)==D3DERR_INVALIDCALL && !device.lights[5].aggregate.get(),
           "failed null SetLight clears in-range stale metadata before validation");
    render::SetEffectsLightAggregate(5,{snapshot,0});
    expect(device.SetLight(256,&light)==D3DERR_INVALIDCALL && device.lights[5].aggregate.snapshot==snapshot,
           "out-of-range SetLight cannot clear another slot");
    // The device starts with kMaxLights slots; an index past them (and past D3D's 256) must not add any.
    const size_t slots=device.lights.size();
    render::SetEffectsLightAggregate(256,{snapshot,0});
    render::SetEffectsLightAggregate(static_cast<uint32_t>(slots),{snapshot,0});
    expect(device.lights.size()==slots,"out-of-range metadata bridge cannot grow device slots");
    device.SetLight(5,&light);
    effects::FfDraw draw;
    draw.lamps=draw.room_light=draw.vertex_ambient_off=true;
    draw.vertex_diffuse_off=draw.vertex_specular_off=0b101001;
    draw.points.u_fxPointCount[0]=2;
    draw.use_unmerged_lights(0b000001);
    expect(draw.vertex_diffuse_off==1 && draw.vertex_specular_off==1 && !draw.lamps && !draw.room_light &&
           draw.points.u_fxPointCount[0]==0 && !draw.vertex_ambient_off,
           "capacity/allocation fallback preserves upgraded sun suppression and restores room-only ambient");
}

void component_shadows(shim::Device &device)
{
    effects::LampShadows lamps;
    effects::Casters casters;
    const auto report = [&](const std::vector<fx::Lamp> &components, const std::vector<render::EffectsLampFlame> &flames) {
#ifdef STORM_LIGHT_COMPONENT_NEGATIVE
        // Old location->shadow route: individual flames, greedy merge, then nearest-centroid reconstruction.
        std::vector<fx::Lamp> raw;
        for (const auto &flame : flames)
        {
            auto lamp = components[flame.component];
            lamp.position = flame.position;
            raw.push_back(lamp);
        }
        lamps.set_lamps(raw);
#else
        lamps.set_lamps(components, flames);
#endif
    };
    std::vector<location::ComponentSource> sources;
    for (uint64_t i = 0; i < 11; ++i)
        sources.push_back({i, {float(i)*.3f, 3, 0}, {1,1,1}, {1,1,1}, 10, {.5f,0,1.5f}, false});
    const auto groups = location::BuildLightComponents(sources);
    std::vector<fx::Lamp> effective;
    std::vector<render::EffectsLampFlame> flames;
    for (const auto &g : groups)
    {
        const auto index = static_cast<uint32_t>(effective.size());
        effective.push_back(fx::Lamp{g.position, g.color});
        for (const size_t i : g.members) flames.push_back({sources[i].position, index});
    }
    report(effective, flames);
    expect(lamps.lamps().size() == 1, "shadow consumer keeps the same transitive component as model lighting");
    expect(lamps.holders().size() == 1 && lamps.holders()[0] >= 1.5f + fx::kLampHolderRadius - 1e-5f,
           "both far chain ends retain explicit fixture membership beyond the merge radius");
    effective[0].color = {0,0,0};
    report(effective, flames);
    expect(lamps.lamps().size() == 1, "instantaneous dark flicker keeps the component");

    const std::array<fx::Vec3,4> floor{{{-2,0,-2},{2,0,-2},{2,0,2},{-2,0,2}}};
    const std::array<uint32_t,6> indices{{0,1,2,0,2,3}};
    render::EffectsLocationGeometry geometry;
    geometry.positions = floor; geometry.indices = indices;
    lamps.set_geometry(geometry);
    fx::Settings settings;
    settings.lamp_shadows = 2; settings.lamp_moving = 0;
    lamps.configure(settings, fx::LampDepth::D24);
    effective = {fx::Lamp{{0,3,0}}, fx::Lamp{{.4f,3,0}}};
    flames = {{{0,3,0},0}, {{.4f,3,0},1}};
    report(effective, flames);
    expect(lamps.lamps().size() == 2, "authoritative separate .4m components are not merged again at legacy .5m radius");
    effects::ShadowCasters volumes;
    lamps.prepare(device, {0,3,2}, casters, volumes);
    expect(lamps.relit({0,3,0}) && lamps.relit({.4f,3,0}), "prepared component positions disable duplicate model point contributions");
    // Query in the interval BEFORE prepare/end_frame reconciles old slots. Shrink/clear must never index stale slots.
    effective.resize(1); flames.resize(1);
    report(effective, flames);
    expect(!lamps.relit({.4f,3,0}), "shrink invalidates relit until slot reconciliation, no stale component index");
    report({}, {});
    expect(!lamps.relit({0,3,0}) && !lamps.relit({.4f,3,0}), "clear rejects stale relit before prepare/end_frame");
    lamps.end_frame();
    volumes.end_frame();
    lamps.shutdown();
}

}

int main()
{
    if (!shim::Context::instance().init_headless(64, 64))
        return 2;
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = params.BackBufferHeight = 64;
    auto *device = new shim::Device(nullptr, params);
    aggregate_transport(*device);
    component_shadows(*device);
    {
        effects::LampShadows lamps;
        effects::Casters casters;
        const std::array<fx::Vec3, 4> positions{{{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}}};
        const std::array<std::array<float, 2>, 4> uvs{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        const std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
        auto *first = make_cutout_texture();
        std::weak_ptr<shim::TexStore> old = first->store;
        render::EffectsLocationGeometry::Surface surface{0, 3, first};
        render::EffectsLocationGeometry location;
        location.positions = positions;
        location.uvs = uvs;
        location.indices = indices;
        location.surfaces = std::span(&surface, 1);
        lamps.set_geometry(location);
        first->Release();
        expect(!old.expired(), "location keeps storage after game releases texture wrapper");

        // Same vertex/index arrays and same number of cutout indices, but a new texture. A count-only comparison
        // silently retained the old surface (and its dangling wrapper) here.
        auto *second = make_cutout_texture();
        std::weak_ptr<shim::TexStore> current = second->store;
        surface.texture = second;
        lamps.set_geometry(location);
        expect(old.expired(), "same-geometry texture replacement releases old storage");
        second->Release();
        expect(!current.expired(), "replacement storage survives its wrapper");
        lamps.set_geometry({});
        expect(current.expired(), "clearing location geometry releases storage");

        // The sun cascade's cut-out casters hold the same storage: a released wrapper never leaves a dangling pointer.
        {
            effects::SunShadows sun;
            auto *cut = make_cutout_texture();
            std::weak_ptr<shim::TexStore> sun_store = cut->store;
            surface.texture = cut;
            surface.first_index = 0;
            sun.set_casters(location);
            cut->Release();
            expect(!sun_store.expired(), "sun casters keep storage after game releases texture wrapper");
            sun.set_casters({});
            expect(sun_store.expired(), "replacing the sun casters releases storage");
            auto *again = make_cutout_texture();
            sun_store = again->store;
            surface.texture = again;
            sun.set_casters(location);
            again->Release();
            sun.shutdown();
            expect(sun_store.expired(), "sun shadow shutdown releases storage");
        }

        auto *ship_texture = make_cutout_texture();
        std::weak_ptr<shim::TexStore> ship_old = ship_texture->store;
        surface = {0, 6, ship_texture};
        lamps.set_ship_geometry(7, 1, {positions, uvs, indices, std::span(&surface, 1)});
        ship_texture->Release();
        expect(!ship_old.expired(), "ship keeps storage after game releases texture wrapper");
        ship_texture = make_cutout_texture();
        std::weak_ptr<shim::TexStore> ship_current = ship_texture->store;
        surface.texture = ship_texture;
        lamps.set_ship_geometry(7, 2, {positions, uvs, indices, std::span(&surface, 1)});
        ship_texture->Release();
        expect(ship_old.expired() && !ship_current.expired(), "ship signature replacement drops old storage and keeps new");
        lamps.set_ship_geometry(7, 3, {});
        expect(ship_current.expired(), "clearing ship geometry releases storage");

        // Populate the per-lantern slot as well as the ship cache. Replacing it with empty geometry must drop
        // both owners immediately; otherwise the obsolete alpha texture survives until that lantern leaves.
        lamps.set_lamps({});
        fx::Settings settings;
        settings.lamp_shadows = 2;
        settings.lamp_moving = 1;
        lamps.configure(settings, fx::LampDepth::D24);
        render::EffectsShip ship;
        ship.id = 7;
        ship.signature = 4;
        ship.world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const fx::Lamp lantern{{0, 3, 0}};
        lamps.set_ship_lights({ship}, {{lantern, ship.id, lantern.position}});
        auto *slotted = make_cutout_texture();
        std::weak_ptr<shim::TexStore> in_slot = slotted->store;
        surface = {0, 6, slotted};
        lamps.set_ship_geometry(7, 4, {positions, uvs, indices, std::span(&surface, 1)});
        effects::ShadowCasters volumes;
        lamps.prepare(*device, {0, 3, 0}, casters, volumes);
        expect(volumes.volumes() == 6 && volumes.volume(0).still == ship.id,
               "ship lantern published its six dynamic faces, apart from the ship's own still geometry");
        lamps.end_frame();
        volumes.end_frame();
        expect(lamps.last_slots() > 0 && lamps.last_static_faces() == 6, "ship lantern submitted its six cached faces on Noop");
        slotted->Release();
        expect(!in_slot.expired(), "drawn ship storage survives its texture wrapper");
        lamps.set_ship_geometry(7, 5, {});
        expect(in_slot.expired(), "empty ship report releases storage held by its drawn slot");

        auto *last = make_cutout_texture();
        std::weak_ptr<shim::TexStore> ending = last->store;
        surface = {0, 6, last};
        lamps.set_geometry(location);
        lamps.set_ship_geometry(7, 4, {positions, uvs, indices, std::span(&surface, 1)});
        last->Release();
        expect(!ending.expired(), "both geometry caches retain their shared storage");
        lamps.shutdown();
        expect(ending.expired(), "renderer shutdown releases all retained storage");
        casters.shutdown();
    }
    device->Release();
    return failures ? 1 : 0;
}
