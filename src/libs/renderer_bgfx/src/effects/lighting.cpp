#include "effects/lighting.hpp"
#include "storm/render/skinned_vertex.hpp"
#include "storm/fx/texture_pixels.hpp"
#include "storm/fx/skin_tone.hpp"

#include "shim/device.hpp"
#include "storm/fx/height_map.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace storm::effects
{

namespace
{
// A surface's light without the sun over its light with it, where no town light ratio says (the game's own
// lightmaps, baked vertex colours): about half of a sunlit bake is the direct sun (the towns' lightmaps measure
// shade / sun 0.4-0.6, fx/shadow_model.hpp SunShadowStrength). An assumption, not a measurement of the surface.
constexpr float kUnmeasuredSunShade = 0.5f;

// The 16 texture units of a lit or baked program (GLES 3.0's minimum): the emulator's stages 0..4, the effects' units
// (fx::kFxStages) and the skinned vertex shaders' bone palette. A second claim of a unit fails here.
constexpr bool FfUnitsDistinct()
{
    uint32_t used = (1u << 5) - 1; // stages 0..4
    for (const uint32_t unit : fx::kFxStages)
    {
        if (unit >= 16 || (used & (1u << unit)))
            return false;
        used |= 1u << unit;
    }
    return shim::kVertexTextureUnit < 16 && !(used & (1u << shim::kVertexTextureUnit));
}
static_assert(FfUnitsDistinct(), "two texture units of the lit and baked programs meet (fx::kFxStages)");

// Absolute world -> view from the device's camera-relative world -> view (row vectors): (p - origin) * view.
std::array<float, 16> AbsoluteView(const std::array<float, 16> &view, const std::array<float, 3> &origin)
{
    std::array<float, 16> v = view;
    for (int j = 0; j < 3; ++j)
        v[12 + j] -= origin[0] * view[j] + origin[1] * view[4 + j] + origin[2] * view[8 + j];
    return v;
}

fx::Vec3 rgb(const D3DCOLORVALUE &c)
{
    return {c.r, c.g, c.b};
}

fx::PointLight point_light(const D3DLIGHT9 &l, const fx::LightFalloff &falloff)
{
    fx::PointLight p;
    p.position = {l.Position.x, l.Position.y, l.Position.z};
    p.color = rgb(l.Diffuse);
    const fx::LightShape shape = fx::ShapeOfGameLight(l.Range, l.Attenuation0, l.Attenuation1, l.Attenuation2, falloff);
    p.range = shape.range;
    p.intensity = shape.intensity;
    p.core = shape.core;
    if (l.Type == D3DLIGHT_SPOT)
    {
        const float length = std::sqrt(l.Direction.x * l.Direction.x + l.Direction.y * l.Direction.y +
                                       l.Direction.z * l.Direction.z);
        if (length > 0.0f)
            p.direction = {l.Direction.x / length, l.Direction.y / length, l.Direction.z / length};
        p.cos_inner = std::cos(l.Theta * 0.5f);
        p.cos_outer = std::cos(l.Phi * 0.5f);
        p.falloff = l.Falloff;
    }
    return p;
}

// The per-draw budget (4 / 8) is a designed limit. Within it every light is taken (the shader applies range and
// cone); beyond it fx::SelectLights ranks them by their contribution at the draw's bounding sphere. `fade`: the
// weakest kept light fades out as the first left-out one catches up (lights that only exist per pixel, so a
// change of the chosen set is never a jump). `picked` marks the chosen entries of `lights`.
fx::LightSelection select(std::span<const fx::PointLight> lights, const fx::Vec3 &center, float radius, int budget,
                          bool fade, uint8_t *picked)
{
    fx::LightSelection selection;
    if (lights.size() <= static_cast<size_t>(budget))
    {
        for (const fx::PointLight &light : lights)
            selection.lights[selection.count++] = light;
        *picked = static_cast<uint8_t>((1u << lights.size()) - 1u);
        return selection;
    }
    selection = fx::SelectLights(lights, center, radius, budget, true, fade);
    *picked = 0;
    for (int i = 0; i < selection.count; ++i)
        *picked |= static_cast<uint8_t>(1u << selection.indices[i]);
    return selection;
}
} // namespace

bool Lighting::aggregate_available() const
{
    const auto *caps = bgfx::getCaps();
    return lighting_ && (flags_ & fx::kLightingPoint) && !aggregate_failed_ && caps &&
           (caps->formats[bgfx::TextureFormat::RGBA32F] & BGFX_CAPS_FORMAT_TEXTURE_2D);
}

std::shared_ptr<const AggregateUpload> Lighting::aggregate_upload(std::span<const fx::AggregateRef> input)
{
    if (input.empty()) return {};
    std::vector<fx::AggregateRef> refs;
    // The common current-snapshot case uploads the whole immutable scene once, shared by all draws.
    const auto snapshot = input.front().snapshot;
    const bool one = std::all_of(input.begin(),input.end(),[&](const auto &r){return r.snapshot==snapshot;});
    if (one)
        for (uint32_t i=0; i<snapshot->groups.size(); ++i) refs.push_back({snapshot,i});
    else
        for (const auto &r : input)
            if (std::find(refs.begin(),refs.end(),r)==refs.end()) refs.push_back(r);
    for (const auto &cached : aggregate_cache_)
        if (cached->payload.refs == refs)
        {
            cached->used_frame = aggregate_frame_;
            return cached;
        }
    const size_t used = static_cast<size_t>(std::count_if(aggregate_cache_.begin(), aggregate_cache_.end(),
        [&](const auto &cached) { return cached->used_frame == aggregate_frame_; }));
    uint8_t reason = 0;
    if (!aggregate_available()) reason = 1;
    else if (used >= fx::kAggregateBundlesPerFrame) reason = 2;
    auto upload = std::make_shared<AggregateUpload>();
    if (!reason && !upload->payload.pack(refs)) reason = 4;
    if (!reason)
    {
        upload->texture = bgfx::createTexture2D(fx::kAggregateTextureSide,fx::kAggregateTextureSide,false,1,
            bgfx::TextureFormat::RGBA32F, BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP,
            bgfx::copy(upload->payload.texels.data(),sizeof(upload->payload.texels)));
        if (!bgfx::isValid(upload->texture)) { aggregate_failed_=true; reason=8; }
    }
    if (reason)
    {
        report_aggregate_fallback(reason, reason == 1 ? "device/mode or prior allocation failure" :
                                          reason == 2 ? "64 bundle variants per frame" :
                                          reason == 4 ? "256 source/group capacity" : "texture allocation failure");
        return {};
    }
    upload->used_frame = aggregate_frame_;
    ++frame_.aggregate_uploads;
    frame_.aggregate_bytes += sizeof(upload->payload.texels);
    aggregate_cache_.push_back(upload);
    return upload;
}

void Lighting::report_aggregate_fallback(uint8_t reason_bit, const char *text)
{
    if (aggregate_reported_ & reason_bit) return;
    aggregate_reported_ |= reason_bit;
    shim::logf("ERROR: compound lights: %s; original unmerged baseline draw, compound shadows disabled", text);
}

void Lighting::prepare_aggregates(FfDraw &draw, const fx::LightSelection &selection, uint8_t directional_mask)
{
    std::vector<fx::AggregateRef> refs;
    auto lamps = draw.lamps ? lamps_.aggregate_refs() : std::array<fx::AggregateRef,fx::kMaxShadowLamps>{};
    for (size_t i=0;i<lamps.size();++i)
        if (draw.aggregate_lamp_overrides[i].get()) lamps[i]=draw.aggregate_lamp_overrides[i];
    for (const auto &r : lamps) if (r.get() && r.get()->count>1) refs.push_back(r);
    for (int i=0; i<selection.count; ++i)
        if (const auto &r=selection.lights[i].aggregate; r.get() && r.get()->count>1) refs.push_back(r);
    if (refs.empty()) return; // singleton exact path has no payload reads/uploads
    uint32_t evaluated_members = 0;
    for (const auto &r : lamps) if (r.get()) evaluated_members += r.get()->count;
    for (int i=0;i<selection.count;++i)
        evaluated_members += selection.lights[i].aggregate.get() ? selection.lights[i].aggregate.get()->count : 1;
    if (evaluated_members <= fx::kAggregateSources) draw.aggregate=aggregate_upload(refs);
    else
        report_aggregate_fallback(16, "a draw exceeds 256 evaluated members including ordinary lights");
    if (!draw.aggregate)
    {
        ++frame_.aggregate_fallbacks;
        const auto fallback=fx::PackUnmergedPointLights(selection,draw.view,draw.vertex_diffuse_off,
                                                       draw.vertex_specular_off,directional_mask);
        draw.use_unmerged_lights(directional_mask); // compounds return to original vertex lights
        draw.points=fallback.points;
        draw.vertex_diffuse_off=fallback.diffuse_off;
        draw.vertex_specular_off=fallback.specular_off;
        return;
    }
    frame_.aggregate_members=std::max(frame_.aggregate_members,evaluated_members); // conservative selected-member bound
    for (size_t i=0;i<lamps.size();++i)
        if (lamps[i].get() && lamps[i].get()->count>1) draw.aggregate_lamps[i]=draw.aggregate->payload.index(lamps[i]);
    for (int i=0;i<selection.count;++i)
        if (const auto &r=selection.lights[i].aggregate; r.get() && r.get()->count>1)
        {
            draw.points.u_fxPoint[4*i+2][1]=draw.aggregate->payload.index(r);
            draw.points.u_fxPoint[4*i+3][3]=selection.lights[i].aggregate_scale;
        }
}

FfBlocks FamilyBlocks(FfFamily family)
{
    switch (family)
    {
    case FfFamily::Lit:
        return {.wet = true};
    case FfFamily::LitLamps:
        return {.lamps = true, .ao = true, .wet = true};
    case FfFamily::Baked:
        return {.town_wall = true, .wet = true};
    case FfFamily::BakedLamps:
        return {.lamps = true, .ao = true, .town_wall = true, .wet = true};
    }
    return {};
}

const char *FamilyName(FfFamily family)
{
    static const char *const names[kFfFamilies] = {"lit", "lit_lamps", "baked", "baked_lamps"};
    return names[static_cast<size_t>(family)];
}

FfFamily ChooseFamily(bool lit, const FfBlocks &needs)
{
    const auto holds = [&needs](FfFamily family) {
        const FfBlocks has = FamilyBlocks(family);
        return (has.lamps || !needs.lamps) && (has.ao || !needs.ao) && (has.town_wall || !needs.town_wall) &&
               (has.wet || !needs.wet);
    };
    // Narrowest first; the last family of each kind holds every block of that kind.
    static constexpr FfFamily kLit[] = {FfFamily::Lit, FfFamily::LitLamps};
    static constexpr FfFamily kBaked[] = {FfFamily::Baked, FfFamily::BakedLamps};
    for (const FfFamily family : lit ? std::span<const FfFamily>(kLit) : std::span<const FfFamily>(kBaked))
        if (holds(family))
            return family;
    // Only a lit draw that reads the town wall gets here (no lit family has it; prepare() never asks for one).
    static bool reported = false;
    if (!reported)
        shim::logf("ERROR: effects: no %s program family holds lamps=%d ao=%d town_wall=%d wet=%d: the widest one draws "
                   "without the rest", lit ? "lit" : "baked", needs.lamps, needs.ao, needs.town_wall, needs.wet);
    reported = true;
    return lit ? FfFamily::LitLamps : FfFamily::BakedLamps;
}

MapReaders DrawReadsMaps(const fx::Settings &settings, bool has_normal, bool character)
{
    const uint32_t flags = settings.lighting_flags;
    const bool lighting = (flags & (fx::kLightingPerPixel | fx::kLightingPoint)) != 0;
    const bool takes = has_normal && (!character || settings.character_relief != fx::CharacterRelief::Off);
    return {takes && lighting && (flags & fx::kLightingNormalMap) != 0, takes && fx::BackgroundProductsFor(settings).gloss};
}

void Lighting::configure(const fx::Settings &settings, const fx::RendererState &state, fx::LampDepth lamp_depth)
{
    settings_ = settings;
    flags_ = settings.lighting_flags;
    lighting_ = (flags_ & (fx::kLightingPerPixel | fx::kLightingPoint)) != 0;
    coverage_ = state.alpha_to_coverage;
    budget_ = fx::PointLightBudget(settings);
    flash_cap_ = lighting_ && budget_ > 0 ? settings.flash_lights : 0;
    if (flash_cap_ == 0)
    {
        flashes_.Clear();
        flash_lights_.clear();
        device_lights_origin_valid_ = false;
    }
    shadows_.configure(settings);
    // Our own lightmaps for the towns with the per-pixel lighting (Средняя / Высокая); Низкая keeps the game's.
    town_light_.set_enabled(fx::BackgroundProductsFor(settings).town_light);
    town_light_.set_debug(settings.town_light_debug);
    lamps_.configure(settings, lamp_depth);
    const fx::BackgroundProducts products = fx::BackgroundProductsFor(settings);
    gloss_ = products.gloss;
    // The relief's material texture (height and gloss) is uploaded when the parallax, the cavity or the gloss reads it.
    normal_maps_.configure(products.relief, settings.normal_map_size,
                           settings.parallax != fx::ParallaxQuality::Off || settings.relief_cavity || gloss_,
                           settings.character_relief != fx::CharacterRelief::Off);
    normal_maps_.configure_gloss(gloss_, settings.gloss_map_size);
    find_shaders();
}

void Lighting::find_shaders()
{
    if (!vs_lit_)
    {
        vs_ff_ = shim::shader_id_by_name("vs_ff");
        vs_lit_ = shim::shader_id_by_name("vs_ff_lit");
        vs_lit_skin_ = shim::shader_id_by_name("vs_ff_lit_skin");
        fs_coverage_ = shim::shader_id_by_name("fs_ff_coverage");
        // fs_ff_<family>[_<suffix>], the suffix letters in the order of CMakeLists.txt: s, n / np, h.
        static const char *const relief[3] = {"", "n", "np"};
        for (size_t f = 0; f < kFfFamilies; ++f)
        {
            const FfFamily family = static_cast<FfFamily>(f);
            for (int s = 0; s < 2; ++s)
                for (int n = 0; n < 3; ++n)
                    for (int h = 0; h < 2; ++h)
                    {
                        const std::string suffix = std::string(s ? "s" : "") + relief[n] + (h ? "h" : "");
                        const std::string name = std::string("fs_ff_") + FamilyName(family) + (suffix.empty() ? "" : "_" + suffix);
                        fs_[f][s][n][h] = shim::shader_id_by_name(name.c_str());
                    }
        }
    }
}

void Lighting::find_ground_shaders()
{
    if (fs_ground_)
        return;
    vs_ground_[0] = shim::shader_id_by_name("vs_ff_dyn");
    vs_ground_[1] = shim::shader_id_by_name("vs_ff_unlit_dyn");
    fs_ground_ = shim::shader_id_by_name("fs_ff_dyn");
    fs_ground_coverage_ = shim::shader_id_by_name("fs_ff_dyn_coverage");
}

bool Lighting::prepare_programs(shim::Device &device, const fx::Settings &settings, int &budget)
{
    find_shaders();
    // The choices of prepare() for these settings; the skinned twins are the shim's (draw.cpp skinned_variant).
    const uint32_t flags = settings.lighting_flags;
    const bool lighting = (flags & (fx::kLightingPerPixel | fx::kLightingPoint)) != 0;
    const bool per_pixel = (flags & fx::kLightingPerPixel) != 0;
    const int specular = per_pixel && (flags & fx::kLightingSpecular) ? 1 : 0;
    // 1: the normal maps, 2: and the parallax variants (draws whose map has no height keep the plain relief ones).
    const bool height = settings.parallax != fx::ParallaxQuality::Off || settings.relief_cavity;
    const int relief = lighting && (flags & fx::kLightingNormalMap) ? (height ? 2 : 1) : 0;
    const int shadow = per_pixel && settings.shadows != fx::ShadowQuality::Off ? 1 : 0;
    if (fx::RendererStateFor(settings).alpha_to_coverage && !device.prepare_program(vs_ff_, fs_coverage_, budget))
        return false;
    if (lighting)
    {
        // The families the draws of these settings can choose (ChooseFamily over what prepare() turns on).
        const fx::BackgroundProducts products = fx::BackgroundProductsFor(settings);
        const bool lamps = per_pixel && products.lamp_shadows, ao = products.ao_volume;
        const int character = settings.character_relief == fx::CharacterRelief::Off ? 0
                              : settings.character_relief == fx::CharacterRelief::Normals ? std::min(relief, 1)
                                                                                          : relief;
        // Baked draws with a gloss map (or every one under the gloss debug view) or wet in the rain take the
        // specular variant.
        const int baked_specular = specular && (settings.gloss || settings.wet_surfaces) ? 1 : 0;
        for (size_t f = 0; f < kFfFamilies; ++f)
        {
            const FfFamily family = static_cast<FfFamily>(f);
            const bool lit = family == FfFamily::Lit || family == FfFamily::LitLamps;
            if ((family == FfFamily::LitLamps || family == FfFamily::BakedLamps) && !lamps && !ao)
                continue;
            for (int n = 0; n <= relief; ++n)
            {
                if (lit)
                {
                    if (!device.prepare_program(vs_lit_, fs_[f][specular][n][shadow], budget))
                        return false;
                    // Characters (the skinned draws): their relief variants with their relief (Settings::character_relief).
                    if (n <= character && !device.prepare_program(vs_lit_skin_, fs_[f][specular][n][shadow], budget))
                        return false;
                    continue;
                }
                for (int s = 0; s <= baked_specular; ++s)
                    if (!device.prepare_program(vs_lit_, fs_[f][s][n][shadow], budget))
                        return false;
            }
        }
    }
    if (per_pixel && settings.lamp_shadows && !casters_.prepare_programs(device, Casters::Pass::Lamp, budget))
        return false;
    if (!per_pixel || settings.shadows == fx::ShadowQuality::Off)
        return true;
    // The plain fixed-function ground under moving casters (FfDraw::ground), lit per vertex or not.
    find_ground_shaders();
    for (const uint32_t vs : vs_ground_)
        if (!device.prepare_program(vs, fs_ground_, budget))
            return false;
    if (fx::RendererStateFor(settings).alpha_to_coverage && !device.prepare_program(vs_ground_[0], fs_ground_coverage_, budget))
        return false;
    return casters_.prepare_programs(device, Casters::Pass::Sun, budget);
}

void Lighting::set_technique(const char *name)
{
    technique_set_ = name && name[0];
    if (!technique_set_)
    {
        material_ = Material::Hull;
        return;
    }
    // CTechnique keeps block names in lower case.
    const auto starts = [name](const char *prefix) { return strncmp(name, prefix, strlen(prefix)) == 0; };
    if (starts("animation"))
        material_ = Material::Character;
    else if (starts("shipsail") || starts("shipflag"))
        material_ = Material::Sail;
    else
        material_ = Material::Stone;
}

void Lighting::prepare(const shim::Device &device, const shim::LayoutInfo &layout, uint32_t cube_variant,
                       const DrawSphereSource &sphere_source, FfDraw &draw)
{
    if (layout.rhw)
        return;
    const DWORD *rs = device.s.rs;
    const shim::TexStore *stage0 = shim::store_of(device.s.textures[0]);
    const shim::TexStore *target = device.rt ? device.rt->store.get() : nullptr;

    // ---- alpha to coverage ----
    const DWORD func = rs[D3DRS_ALPHAFUNC];
    const bool coverage = coverage_ && rs[D3DRS_ALPHATESTENABLE] && !rs[D3DRS_ALPHABLENDENABLE] &&
                          (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL) && target && target->target_flags != 0 &&
                          stage0 && stage0->kind == shim::TexKind::Tex2D && device.s.tss[0][D3DTSS_COLOROP] != D3DTOP_DISABLE;

    // ---- lighting ----
    const bool lit_state = rs[D3DRS_LIGHTING] != 0;
    const bool per_pixel = (flags_ & fx::kLightingPerPixel) != 0;
    bool lit = lighting_ && layout.has_normal && lit_state;
    // The room's lamps relight what is drawn into the game picture, each with its shadow (effects/lamp_shadows.hpp):
    // baked location geometry (which then takes the baked variant) and lit models. The first receiver of a frame
    // gives the lamps around its camera their slots.
    const shim::TexStore *picture = device.back_color ? device.back_color->store.get() : nullptr;
    bool lamp_on = false;
    if (lamps_.enabled() && lighting_ && layout.has_normal && target && target == picture)
    {
        const D3DMATRIX &v = device.s.view;
        std::array<float, 3> eye{}; // absolute, like the lamps
        for (int j = 0; j < 3; ++j)
            eye[j] = origin_[j] - (v.m[3][0] * v.m[j][0] + v.m[3][1] * v.m[j][1] + v.m[3][2] * v.m[j][2]);
        lamp_on = lamps_.prepare(const_cast<shim::Device &>(device), eye, casters_, shadow_casters_);
    }
    // Baked colours: dynamic lights only (the lighter tool put the location's own lights into the colours).
    fx::LightSelection baked_lights;
    uint8_t unused = 0;
    bool baked = !lit_state && lamp_on;
    // The dynamic and flash lights are absolute; the draw's sphere and view are in the device's camera-relative world.
    if (!device_lights_origin_valid_ && (!dynamic_lights_.empty() || !flash_lights_.empty()))
    {
        device_flash_lights_ = flash_lights_;
        for (fx::PointLight &l : device_flash_lights_)
            for (int k = 0; k < 3; ++k)
                l.position[k] -= origin_[k];
        device_dynamic_lights_ = dynamic_lights_;
        for (fx::PointLight &l : device_dynamic_lights_)
            for (int k = 0; k < 3; ++k)
                l.position[k] -= origin_[k];
        device_dynamic_lights_.insert(device_dynamic_lights_.end(), device_flash_lights_.begin(), device_flash_lights_.end());
        device_lights_origin_valid_ = true;
    }
    if (lighting_ && layout.has_normal && !lit_state && budget_ > 0 && (!dynamic_lights_.empty() || !flash_lights_.empty()))
    {
        // The candidates of this draw: a carried lantern that has a lamp slot is lit by the lamps, with its shadow
        // (where the lamps light this draw), so it is not added a second time. The flashes follow them (no lamp has one).
        candidate_lights_.clear();
        for (size_t i = 0; i < dynamic_lights_.size(); ++i)
            if (!lamp_on || !lamps_.relit(dynamic_lights_[i].position))
                candidate_lights_.push_back(device_dynamic_lights_[i]);
        const size_t first_flash = candidate_lights_.size();
        candidate_lights_.insert(candidate_lights_.end(), device_flash_lights_.begin(), device_flash_lights_.end());
        // Ranked at the draw's own vertices: a torch lights the wall next to it wherever the camera is.
        if (const DrawSphere &sphere = sphere_source.get(); sphere.valid)
        {
            if (!candidate_lights_.empty())
                baked_lights = select(candidate_lights_, sphere.center, sphere.radius, budget_, true, &unused);
            baked |= baked_lights.count > 0;
            // select() takes every candidate within the budget, else ranks them (SelectLights' indices).
            const bool all_taken = candidate_lights_.size() <= static_cast<size_t>(budget_);
            for (int i = 0; i < baked_lights.count; ++i)
                if ((all_taken ? static_cast<size_t>(i) : baked_lights.indices[i]) >= first_flash)
                {
                    ++frame_.flashed;
                    break;
                }
        }
        else if (!bounds_reported_)
        {
            bounds_reported_ = true;
            shim::logf("effects: a baked draw without vertex positions gets no dynamic lights");
        }
    }
    // Relief: the normal map of stage 1's texture (a grey height texture: the game's 2x detail layer, on the second UV
    // set) or else of stage 0's (the colour texture of stone, wood, plaster, ground).
    bgfx::TextureHandle normal_map = BGFX_INVALID_HANDLE;
    float relief_fade = 0.0f; // a map just uploaded fades in (effects::kReliefFade)
    int relief_stage = -1;
    NormalMaps::Relief relief_map;
    // Characters: with Settings::character_relief, faded out on skin and hair per vertex (ff_fragment.sh ffSkin).
    const bool character = lit && material_ == Material::Character;
    const MapReaders reads = DrawReadsMaps(settings_, layout.has_normal, character);
    if (reads.relief && device.s.tss[0][D3DTSS_COLOROP] != D3DTOP_DISABLE)
    {
        const auto enabled_store = [&](int stage) {
            return device.s.tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE ? shim::store_of(device.s.textures[stage]) : nullptr;
        };
        const auto selected = normal_maps_.select(enabled_store(0), enabled_store(1), fx_);
        relief_map = selected.map;
        draw.relief_entry = selected.entry;
        normal_map = relief_map.handle;
        relief_fade = relief_map.fade;
        relief_stage = selected.stage;
    }
    baked |= !lit_state && bgfx::isValid(normal_map);
    // Gloss: stage 0's colour texture's: the G of its relief's material texture when its relief class has a height
    // (relief_store.hpp), else its own gloss map (characters: only with their relief, faded out on skin).
    NormalMaps::Relief gloss_map;
    fx::GlossClass gloss_class = fx::GlossClass::None;
    bool metal_mask = false; // the material's R is a metal mask (a gloss map's), not a relief's height
    shim::TexStore *gloss_store = shim::store_of(device.s.textures[0]);
    if (reads.gloss && gloss_store && device.s.tss[0][D3DTSS_COLOROP] != D3DTOP_DISABLE)
    {
        if (gloss_store->normal_map && gloss_store->normal_map->material_gloss != fx::GlossClass::None)
        {
            const NormalMaps::Relief material = relief_stage == 0 ? relief_map : normal_maps_.handle(*gloss_store, fx_);
            gloss_map = {material.material, material.fade};
            gloss_class = gloss_store->normal_map->material_gloss;
        }
        else if (gloss_store->gloss_map)
        {
            gloss_map = normal_maps_.gloss(*gloss_store, fx_);
            gloss_class = gloss_store->gloss_map->gloss;
            metal_mask = true;
        }
    }
    const bool gloss_debug = gloss_ && settings_.gloss_debug != 0;
    baked |= !lit_state && (bgfx::isValid(gloss_map.handle) || gloss_debug) && target && target == picture;
    // The interior's AO volume darkens what is drawn into the game picture: baked location geometry (which then takes
    // the baked variant) and lit models (characters, props). Reflections and other targets keep their look.
    const bool ao = ao_volume_ && lighting_ && layout.has_normal && target && target == picture;
    baked |= !lit_state && ao;
    // Under a location lightmap (relit by the town light's ratio, TownLight::ratio): no cascade, see below.
    bool lightmapped = false;
    const shim::TexStore *lightmap = nullptr;
    for (int stage = 0; stage < 4 && !lightmapped; ++stage)
        if (device.s.tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE)
            if (const shim::TexStore *store = shim::store_of(device.s.textures[stage]); store && store->lightmap)
            {
                lightmapped = true;
                lightmap = store;
            }
    // The moving casters' sun shadows on a surface with its own static light (a lightmap, baked colours): an opaque
    // unskinned draw into the game picture outdoors that a caster's shadow can reach. Under a relit lightmap its
    // tone says how much of an open texel's light is the sun's; elsewhere kUnmeasuredSunShade stands in. The static
    // layer keeps the shadow out of the place's own shade.
    const auto ground_receiver = [&]() {
        if (!shadows_.enabled() || !weather_sun_.present || !target || target != picture || lamps_.interior() ||
            layout.skinned || cube_variant != 0 || !rs[D3DRS_ZWRITEENABLE] || rs[D3DRS_ALPHABLENDENABLE] ||
            device.s.proj.m[2][3] == 0.0f)
            return false;
        memcpy(draw.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
        if (!shadows_.prepare(const_cast<shim::Device &>(device), draw.view, origin_, casters_, shadow_casters_) ||
            !shadows_.casting())
            return false; // before the draw's bounds: nothing moves under the sun in most frames of most places
        const DrawSphere &sphere = sphere_source.get();
        if (!sphere.valid || !shadows_.shaded(sphere.center, sphere.radius))
            return false;
        draw.shadow = true;
        draw.sun_receiver.ground = true;
        draw.sun_receiver.shade = town_light_.sun_shade(lightmap, kUnmeasuredSunShade);
        ++frame_.ground;
        return true;
    };
    // A town's walls and props without a lightmap, drawn with the game's baked colours into the game picture: our
    // sun and sky relight them as the town light relights the lightmapped ground (town_light.hpp TownWallTone).
    const bool town_wall = per_pixel && lighting_ && layout.has_normal && !lit_state && !lightmapped && target &&
                           target == picture && !lamps_.interior() && settings_.town_walls && town_light_.walls_active();
    baked |= town_wall;
    // Rain: the outdoor surfaces drawn into the game picture are wet (characters and sails keep their look); baked
    // location geometry then takes the baked variant.
    const bool wet = settings_.wet_surfaces && wet_.level > 0.0f && lighting_ && layout.has_normal && target &&
                     target == picture && !lamps_.interior() &&
                     !(lit && (material_ == Material::Character || material_ == Material::Sail));
    baked |= !lit_state && wet;
    if (!lit && !baked && !coverage)
    {
        draw.ground = ground_receiver(); // the plain program's twin with the dynamic layer's tap
        return;
    }
    if (cube_variant != 0)
    {
        if (!cube_reported_)
            shim::logf("effects: cube-mapped fixed-function draws have no lighting / coverage variant; they keep "
                       "vertex lighting and the alpha test");
        cube_reported_ = true;
        return;
    }

    draw.active = true;
    draw.coverage = {stage0 ? static_cast<float>(stage0->width) : 1.0f, stage0 ? static_cast<float>(stage0->height) : 1.0f,
                     coverage ? 1.0f : 0.0f, 0.0f};
    if (coverage)
    {
        draw.state = BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        ++frame_.coverage;
    }
    if (!lit && !baked)
    {
        draw.vs_id = vs_ff_;
        draw.fs_id = fs_coverage_;
        // The game draws its location geometry with the alpha test on: the ground under a moving caster is here.
        if (ground_receiver())
        {
            find_ground_shaders();
            draw.ground = true;
            draw.vs_id = vs_ground_[0];
            draw.fs_id = fs_ground_coverage_;
        }
        return;
    }
    draw.lit = true;
    draw.vs_id = vs_lit_;
    // A preset with sun shadows draws every lit and baked draw with its shadowed program (one program per draw kind
    // to prepare, not two): the draws that receive nothing bind the map unshadowed (submit()).
    draw.shadow_variant = per_pixel && shadows_.enabled();
    if (ao)
    {
        draw.ao = true;
        ++frame_.ao;
    }
    if (lamp_on)
    {
        draw.lamps = true;
        ++frame_.lamp_lit;
    }
    if (wet)
    {
        draw.wet = {{{wet_.level, settings_.wet_surfaces >= 2 ? 1.0f : 0.0f, 0.0f, 0.0f},
                     {wet_.sky[0], wet_.sky[1], wet_.sky[2], 0.0f}}};
        ++frame_.wet;
    }
    // Compound fallback also needs absolute view-to-world with lamp shadows disabled.
    {
        memcpy(draw.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
        draw.origin = origin_;
        draw.world_view = AbsoluteView(draw.view, origin_);
    }


    // Sun and ambient as the fixed-function pipeline would use them; ComputeLighting gives the per-pixel terms.
    fx::LightingInput input;
    memcpy(input.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
    D3DCOLORVALUE ambient;
    {
        const DWORD c = rs[D3DRS_AMBIENT];
        ambient = {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1.0f};
    }
    input.ambient = rgb(ambient);
    fx::PointLight points[shim::kMaxLights];
    uint8_t point_slot[shim::kMaxLights];
    size_t point_count = 0;
    int sun_slot = -1;
    const auto lamp_refs = lamp_on ? lamps_.aggregate_refs() : std::array<fx::AggregateRef,fx::kMaxShadowLamps>{};
    // A restored stateblock owns its RGB, not merely a live group ID. Reuse current faces only for unchanged geometry.
    for (const auto &entry : device.lights)
        if (entry.enabled && entry.aggregate.get())
            for (const auto &ref : lamp_refs)
                if (ref.get() && ref.get()->id==entry.aggregate.get()->id &&
                    !fx::SameAggregateGeometry(ref,entry.aggregate))
                    draw.lamps=false;
    uint32_t slot = 0;
    for (const shim::Light &entry : device.lights)
    {
        if (!entry.enabled || slot >= shim::kMaxLights)
            continue;
        const D3DLIGHT9 &l = entry.light;
        if (l.Type == D3DLIGHT_DIRECTIONAL)
        {
            if (sun_slot < 0)
            {
                sun_slot = static_cast<int>(slot);
                const float length = std::sqrt(l.Direction.x * l.Direction.x + l.Direction.y * l.Direction.y +
                                               l.Direction.z * l.Direction.z);
                if (length > 0.0f)
                    input.sun_dir = {-l.Direction.x / length, -l.Direction.y / length, -l.Direction.z / length};
                input.sun_diffuse = rgb(l.Diffuse);
            }
        }
        else if (draw.lamps && l.Type == D3DLIGHT_POINT &&
                 (entry.aggregate.get() ? lamps_.relit(entry.aggregate) :
                  lamps_.relit({l.Position.x + origin_[0], l.Position.y + origin_[1], l.Position.z + origin_[2]})))
        {
            // A captured older flicker frame overrides this lamp's payload for this draw only.
            if (entry.aggregate.get())
                for (size_t i=0;i<lamp_refs.size();++i)
                    if (lamp_refs[i].get() && lamp_refs[i].get()->id==entry.aggregate.get()->id)
                        draw.aggregate_lamp_overrides[i]=entry.aggregate;
            // One of the room's lamps: the receiver lights it per pixel from the lamp grid, with its shadow.
            draw.vertex_diffuse_off |= static_cast<uint8_t>(1u << slot);
            draw.vertex_specular_off |= static_cast<uint8_t>(1u << slot);
        }
        else
        {
            points[point_count] = point_light(l, settings_.light_falloff);
            points[point_count].aggregate = entry.aggregate;
            points[point_count].device_slot = static_cast<uint8_t>(slot);
            points[point_count].aggregate_origin = origin_;
            if (const auto *g = entry.aggregate.get())
            {
                points[point_count].range = g->support;
                points[point_count].intensity = g->shape.intensity;
                points[point_count].core = g->shape.core;
            }
            point_slot[point_count++] = static_cast<uint8_t>(slot);
        }
        ++slot;
    }

    fx::LightingMaterial material = fx::LightingMaterial::Stone();
    bool two_sided = false;
    if (lit)
        switch (material_)
        {
        case Material::Hull:
            material = fx::LightingMaterial::Hull();
            break;
        case Material::Sail:
            material = fx::LightingMaterial::Sail();
            two_sided = rs[D3DRS_CULLMODE] == D3DCULL_NONE;
            break;
        case Material::Character:
            material = fx::LightingMaterial::Character();
            break;
        case Material::Stone:
            break;
        }
    draw.light = fx::ComputeLighting(input, material, sky_, settings_);
    auto &u = draw.light.u_fxLight;
    const bool hemisphere = per_pixel && (flags_ & fx::kLightingHemisphere);
    const bool specular = per_pixel && (flags_ & fx::kLightingSpecular);
    // The per-pixel sun reaches a pixel by how open it is (the visibility volume, ff_fragment.sh): under a roof none
    // of it arrives, whatever the scripts say of the place's weather.
    if (!lit && per_pixel && (bgfx::isValid(gloss_map.handle) || wet) && weather_sun_.present)
    {
        // A glossy (or wet) baked surface: the bake holds the sun's light, its highlight is added per pixel
        // (ff_fragment.sh, by how open the pixel is) from the weather's sun (a baked draw has the fixed-function lights
        // off).
        fx::LightingInput sun = input;
        sun.sun_dir = {weather_sun_.dir[0], weather_sun_.dir[1], weather_sun_.dir[2]};
        sun.sun_diffuse = {weather_sun_.color[0], weather_sun_.color[1], weather_sun_.color[2]};
        const fx::LightingUniforms from_weather = fx::ComputeLighting(sun, material, sky_, settings_);
        for (int k = 0; k < 3; ++k)
        {
            u[0][k] = from_weather.u_fxLight[0][k];
            u[1][k] = from_weather.u_fxLight[1][k];
        }
    }
    else if (!per_pixel || sun_slot < 0 || !lit)
        u[1][0] = u[1][1] = u[1][2] = 0.0f; // the sun stays in the vertex sum (or there is none)
    else
    {
        draw.vertex_diffuse_off |= static_cast<uint8_t>(1u << sun_slot);
        if (specular)
            draw.vertex_specular_off |= static_cast<uint8_t>(1u << sun_slot);
    }
    // Lit models of an interior stand in the room's own light (the baked colours around them, ff_fragment.sh
    // ffRoomLight) instead of the weather's ambient.
    draw.hemisphere_ambient = hemisphere && lit;
    // Baked draws read it too: the AO volume takes only the occlusion their bake lacks against it (ff_fragment.sh).
    draw.room_light = draw.lamps && lamps_.room_light();
    if ((hemisphere || draw.room_light) && lit)
        draw.vertex_ambient_off = true;
    if (lit && draw.room_light && !hemisphere)
        // An interior model's ambient where the sky is seen (ff_fragment.sh, over the room light): the weather's, flat.
        for (int i = 0; i < 3; ++i)
            u[3][i] = u[4][i] = input.ambient[i];
    else if (!hemisphere || !lit)
        for (int i = 0; i < 3; ++i)
            u[3][i] = u[4][i] = 0.0f;
    if (!per_pixel || !(flags_ & fx::kLightingTranslucent))
        u[2][3] = 0.0f;
    if (!per_pixel || !(flags_ & fx::kLightingRim))
        u[5][1] = 0.0f;
    u[5][0] *= relief_fade * (character ? settings_.character_relief_strength : 1.0f);
    const bool has_relief = bgfx::isValid(normal_map) && u[5][0] > 0.0f;
    // Generated gloss can remain ready while the normal map is pending or its strength is zero. Protect torso skin
    // in that specular-only variant too; its colour always comes from the stage-0 albedo, independently of relief UVs.
    if ((has_relief || bgfx::isValid(gloss_map.handle)) && character && layout.skinned && stage0 &&
        settings_.character_skin_colour)
        draw.skin_tone = skin_tone(*shim::store_of(device.s.textures[0]), sphere_source);
    draw.ff = {two_sided && u[2][3] > 0.0f ? 1.0f : 0.0f, has_relief ? static_cast<float>(relief_stage) : 0.0f, 0.0f, 0.0f};
    int relief = 0; // the variant: 1 the normal map, 2 with the parallax
    if (has_relief)
    {
        relief = 1;
        draw.normal_map = normal_map;
        draw.normal_map_flags = device.sampler_flags(relief_stage, *shim::store_of(device.s.textures[relief_stage]));
        ++frame_.relief;
        // The parallax: the relief stage's map with its height (stage 0's colour texture, or stage 1's grey height
        // texture with its own heights), on coordinates that are not projected (the variant reads that stage where the
        // relief is); its depth fades in with the relief.
        // A clamp-addressed stage (a texture drawn once over its polygon, not tiled) has none: the shifted reads would
        // run off its edge into the clamped border.
        const uint32_t u = draw.normal_map_flags & BGFX_SAMPLER_U_MASK, v = draw.normal_map_flags & BGFX_SAMPLER_V_MASK;
        const bool clamped = u == BGFX_SAMPLER_U_CLAMP || u == BGFX_SAMPLER_U_BORDER || v == BGFX_SAMPLER_V_CLAMP ||
                             v == BGFX_SAMPLER_V_BORDER;
        // The cavity needs no shift: it also draws on a clamped stage 0 or with the parallax off.
        // Zero strength must disable the UV steps too: the shader's world-depth floor would otherwise turn a
        // submitted zero depth back into visible parallax. The cavity can still use the unshifted material below.
        const bool parallax = settings_.parallax != fx::ParallaxQuality::Off && settings_.parallax_strength > 0.0f &&
                              !clamped && relief_map.parallax &&
                              (!character || (settings_.character_relief == fx::CharacterRelief::Parallax &&
                                              settings_.character_parallax_depth > 0.0f));
        const bool cavity = settings_.relief_cavity && (!character || settings_.character_relief == fx::CharacterRelief::Parallax);
        if (bgfx::isValid(relief_map.material) && relief_map.depth > 0.0f && (parallax || cavity) &&
            !(device.s.tss[relief_stage][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED))
        {
            relief = 2;
            draw.parallax_variant = true;
            draw.parallax_untrusted = settings_.parallax != fx::ParallaxQuality::Off && !clamped && !relief_map.parallax;
            draw.material = relief_map.material;
            draw.material_flags = draw.normal_map_flags;
            // The class's own depth (stored with the height, fx::ReliefDepthTexels), the player's strength, the fade-in.
            const float depth = relief_map.depth * std::clamp(settings_.parallax_strength, 0.0f, 3.0f) * relief_fade *
                                (character ? std::clamp(settings_.character_parallax_depth, 0.0f, 10.0f) : 1.0f);
            const bool occlusion = parallax && settings_.parallax == fx::ParallaxQuality::Occlusion;
            // The relief's own shadow towards the sun: occlusion mapping only, not indoors (no sun shines there).
            // Not on characters: their parallax is shallow, a shadow inside it would only crawl as they move.
            const bool self_shadow = occlusion && settings_.parallax_shadow && !lamps_.interior() && !character;
            draw.parallax = {depth / static_cast<float>(relief_map.width), depth / static_cast<float>(relief_map.height_texels),
                             // Characters: 8 steps at every angle (their relief is two texels deep).
                             !parallax ? 0.0f : occlusion ? (character ? 8.0f : static_cast<float>(fx::kParallaxSteps)) : 1.0f,
                             std::log2(static_cast<float>(std::max(relief_map.width, relief_map.height_texels))),
                             self_shadow ? fx::kParallaxShadow : 0.0f, settings_.parallax_debug ? 1.0f : 0.0f,
                             cavity ? fx::kReliefCavity : 0.0f,
                             fx::ReliefParallaxMaxDepth(draw.relief_entry ? draw.relief_entry->relief : fx::ReliefClass::None)};
            if (draw.has_parallax())
                ++frame_.parallax;
        }
    }
    // Gloss: the map's texels shape the highlight; u_fxGloss.w switches on the lamps' and the baked surfaces'
    // highlights (develop's look without gloss: the lit materials' fixed highlight only).
    // The parallax's material, when it is bound, is the same texture (stage 0's relief); a stage 1 parallax binds stage
    // 1's at the same unit: stage 0's gloss yields to it (counted in the census).
    const bool gloss_yields = draw.parallax_variant && relief_stage == 1 && bgfx::isValid(gloss_map.handle);
    if (gloss_yields)
        ++frame_.gloss_yields;
    const bool glossy = !gloss_yields && bgfx::isValid(gloss_map.handle) && gloss_map.fade > 0.0f;
    if (glossy)
    {
        draw.material = gloss_map.handle;
        draw.material_flags = device.sampler_flags(0, *gloss_store);
        ++frame_.glossy;
    }
    // The metal tint: by a gloss map's metal mask (its R, u_fxGloss.y > 0); a relief's material has its height in R,
    // so there by its polished texels, their gloss (u_fxGloss.y < 0, ff_fragment.sh).
    const float metal_tint = fx::GlossMetalTint(gloss_class);
    draw.gloss = {glossy ? gloss_map.fade : 0.0f, glossy ? (metal_mask ? metal_tint : -metal_tint) : 0.0f,
                  gloss_ ? static_cast<float>(settings_.gloss_debug) : 0.0f, gloss_ ? settings_.gloss_strength : 0.0f};
    // Wet baked surfaces take the highlights too (their gloss is the rain's, ff_wet.sh).
    const bool baked_specular = specular && ((gloss_ && (glossy || gloss_debug)) || wet);

    if (lit)
    {
        // Flashes first, up to half of the budget (they exist only per pixel: their choice fades); the D3D point lights
        // take what is left, the rest of them stays in the vertex sum.
        const D3DMATRIX &w = device.s.world;
        const DrawSphere &sphere = sphere_source.get();
        const fx::Vec3 center = sphere.valid ? sphere.center : fx::Vec3{w.m[3][0], w.m[3][1], w.m[3][2]};
        fx::LightSelection flashes;
        if (!flash_lights_.empty() && budget_ > 1)
        {
            flashes = fx::SelectLights(device_flash_lights_, center, sphere.valid ? sphere.radius : 0.0f, budget_ / 2, true, true);
            if (flashes.count > 0)
                ++frame_.flashed;
        }
        fx::LightSelection selection;
        if (budget_ > 0 && point_count > 0)
        {
            uint8_t picked = 0;
            // Lights left out stay in the vertex sum, so the choice is not faded; it depends only on where the
            // draw's vertices and the lights are. Without positions the model origin stands for the draw.
            if (!sphere.valid && !bounds_reported_)
            {
                bounds_reported_ = true;
                shim::logf("effects: a lit draw without vertex positions: its lights are chosen at the model origin");
            }
            const int exact_budget = std::any_of(points, points+point_count, [](const auto &p){return p.aggregate.get();}) ?
                                     fx::kMaxPointLights : budget_;
            selection = select(std::span(points, point_count), center, sphere.valid ? sphere.radius : 0.0f,
                               exact_budget - flashes.count, false, &picked);
            for (size_t i = 0; i < point_count; ++i)
                if (picked & (1u << i))
                {
                    draw.vertex_diffuse_off |= static_cast<uint8_t>(1u << point_slot[i]);
                    if (specular)
                        draw.vertex_specular_off |= static_cast<uint8_t>(1u << point_slot[i]);
                }
            selection.dropped = 0; // the rest is lit per vertex
        }
        for (int i = 0; i < flashes.count && selection.count < fx::kMaxPointLights; ++i)
            selection.lights[selection.count++] = flashes.lights[i];
        draw.points = fx::PackPointLights(selection, input.view);
        prepare_aggregates(draw, selection, sun_slot >= 0 ? static_cast<uint8_t>(1u<<sun_slot) : 0);
        // Sun shadows: lit draws into the game picture (the first one of a frame prepares the map with its camera).
        // Surfaces under a location lightmap carry the sun's shadows already (ours: the same sun, TownLight): they
        // do not receive. (Where one walks under a roof no sun shadow map is made: the sun reaches a pixel there by
        // its openness.) The same draws are what the sun's dynamic layer takes as casters (sun_lit()).
        if (per_pixel && sun_slot >= 0 && target && target == picture && !lamps_.interior() && !lightmapped)
        {
            memcpy(draw.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
            draw.shadow = shadows_.prepare(const_cast<shim::Device &>(device), draw.view, origin_, casters_, shadow_casters_);
        }
        draw.family = ChooseFamily(true, {.lamps = draw.lamps, .ao = draw.ao, .wet = wet});
        draw.fs_id = fs_[static_cast<size_t>(draw.family)][specular][relief][draw.shadow_variant ? 1 : 0];
        frame_.shadowed += draw.shadow ? 1 : 0;
        ++frame_.lit;
    }
    else
    {
        draw.points = fx::PackPointLights(baked_lights, input.view);
        prepare_aggregates(draw, baked_lights);
        // Both geometry mismatch and upload failure can disable the lamps after the original carried-light filter.
        // Reselect the complete independent list so a torch formerly represented by its own shadow slot survives.
        if (lamp_on && !draw.lamps && budget_ > 0 && !dynamic_lights_.empty())
            if (const DrawSphere &sphere=sphere_source.get(); sphere.valid)
            {
                uint8_t ignored=0;
                const auto raw=select(device_dynamic_lights_,sphere.center,sphere.radius,budget_,true,&ignored);
                draw.points=fx::PackPointLights(raw,input.view);
            }
        if (town_wall)
        {
            draw.town_wall = true;
            ++frame_.town_walls;
            // They receive both layers (the place's still geometry casts into the static one from its tiles).
            if (settings_.shadows != fx::ShadowQuality::Off && weather_sun_.present)
            {
                memcpy(draw.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
                draw.shadow = shadows_.prepare(const_cast<shim::Device &>(device), draw.view, origin_, casters_, shadow_casters_);
                const DrawSphere &sphere = sphere_source.get();
                draw.sun_receiver.dynamic_reaches = draw.shadow && (!sphere.valid || shadows_.shaded(sphere.center, sphere.radius));
            }
            if (draw.view == std::array<float, 16>{})
                memcpy(draw.view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
        }
        else
            ground_receiver();
        frame_.shadowed += draw.shadow ? 1 : 0;
        draw.family = ChooseFamily(false, {.lamps = draw.lamps, .ao = draw.ao, .town_wall = draw.town_wall, .wet = wet});
        draw.fs_id = fs_[static_cast<size_t>(draw.family)][baked_specular ? 1 : 0][relief][draw.shadow_variant ? 1 : 0];
        ++frame_.baked;
    }
}

const char *Lighting::ParallaxMissName(ParallaxMiss miss)
{
    static const char *const names[] = {"no relief by its class", "ported shader", "decal", "pretransformed",
                                        "no vertex normals", "cube map", "character", "switched off",
                                        "stage 1 height texture without a height", "map not ready", "map without height",
                                        "projected coordinates", "clamp-addressed", "no relief on the draw",
                                        "its class takes none, too few trusted texels or excluded", "other"};
    static_assert(std::size(names) == kParallaxMisses);
    return names[static_cast<int>(miss)];
}

void Lighting::census(const shim::Device &device, const shim::LayoutInfo &layout, bool ported, bool decal,
                      uint32_t cube_variant, const FfDraw &draw)
{
    const shim::TexStore *s0 = shim::store_of(device.s.textures[0]);
    if (!s0 || s0->kind != shim::TexKind::Tex2D || device.s.tss[0][D3DTSS_COLOROP] == D3DTOP_DISABLE)
        return; // untextured
    // The relief's stage: stage 1's grey height texture when it took the draw, else stage 0's colour texture.
    const shim::TexStore *s1 = shim::store_of(device.s.textures[1]);
    const bool stage1 = s1 && s1->normal_map && device.s.tss[1][D3DTSS_COLOROP] != D3DTOP_DISABLE &&
                        (!draw.relief_entry || draw.relief_entry == s1->normal_map.get());
    const int stage = stage1 ? 1 : 0;
    const shim::TexStore &texture = stage1 ? *s1 : *s0;
    if (draw.has_parallax())
    {
        // Only actual offset/POM submission, not the shared cavity-only shader variant.
        cover(texture, texture.normal_map->relief, kCoverageParallax);
        return;
    }
    const uint32_t flags = device.sampler_flags(stage, texture);
    const uint32_t u = flags & BGFX_SAMPLER_U_MASK, v = flags & BGFX_SAMPLER_V_MASK;
    const NormalMapEntry *entry = texture.normal_map.get();
    ParallaxMiss miss = ParallaxMiss::Other;
    if (!entry)
        miss = ParallaxMiss::NoReliefClass;
    else if (ported)
        miss = ParallaxMiss::Ported;
    else if (decal)
        miss = ParallaxMiss::Decal;
    else if (layout.rhw)
        miss = ParallaxMiss::Pretransformed;
    else if (!layout.has_normal)
        miss = ParallaxMiss::NoNormals;
    else if (cube_variant != 0)
        miss = ParallaxMiss::Cube;
    else if (device.s.rs[D3DRS_LIGHTING] && material_ == Material::Character &&
             settings_.character_relief != fx::CharacterRelief::Parallax)
        miss = ParallaxMiss::Character;
    else if (!lighting_ || !(flags_ & fx::kLightingNormalMap) ||
             (settings_.parallax == fx::ParallaxQuality::Off || settings_.parallax_strength <= 0.0f))
        miss = ParallaxMiss::Off;
    else if (entry->state != NormalMapEntry::Uploaded)
        miss = ParallaxMiss::NotReady;
    else if (!bgfx::isValid(entry->material) || entry->depth <= 0.0f)
        miss = stage1 ? ParallaxMiss::Stage1 : ParallaxMiss::NoHeight;
    else if (device.s.tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED)
        miss = ParallaxMiss::Projected;
    else if (u == BGFX_SAMPLER_U_CLAMP || u == BGFX_SAMPLER_U_BORDER || v == BGFX_SAMPLER_V_CLAMP || v == BGFX_SAMPLER_V_BORDER)
        miss = ParallaxMiss::Clamped;
    else if (!bgfx::isValid(draw.normal_map))
        miss = ParallaxMiss::NoRelief;
    else if (draw.parallax_untrusted)
        miss = ParallaxMiss::NotTrusted;
    ++frame_.parallax_miss[static_cast<size_t>(miss)];
    cover(texture, entry ? entry->relief : fx::ReliefClass::None, static_cast<uint8_t>(miss));
}

void Lighting::cover(const shim::TexStore &texture, fx::ReliefClass relief, uint8_t status)
{
    if (texture.name.empty())
        return; // not a game file (render targets, the renderer's own textures)
    auto [it, added] = parallax_coverage_.try_emplace(texture.serial);
    Coverage &c = it->second;
    if (added)
        c.name = texture.name;
    // A texture that drew with the parallax in the window counts with it (its other draws may be clamped, decals...).
    const bool parallax = status == kCoverageParallax;
    if (parallax)
        c.parallax_frame = census_frame_;
    c.seen |= 1u << (parallax ? 31 : status);
    if ((added || c.relief != relief) && relief == fx::ReliefClass::None)
        c.rule = fx::ClassifyTextureName(c.name).rule;
    c.relief = relief;
    c.status = !parallax && c.parallax_frame && census_frame_ - c.parallax_frame < kCoverageFrames ? kCoverageParallax
                                                                                                   : status;
    c.frame = census_frame_;
}

// One line per texture whose set of reasons changed since its last line (a texture drawn with two reasons in a window
// is one line, not one per draw).
void Lighting::log_texture(Coverage &c)
{
    std::string reasons;
    if (c.seen & (1u << 31))
        reasons = "parallax";
    for (size_t i = 0; i < kParallaxMisses; ++i)
        if (c.seen & (1u << i))
            reasons += std::string(reasons.empty() ? "" : ", ") + "no parallax: " + ParallaxMissName(static_cast<ParallaxMiss>(i));
    shim::logf("effects parallax: texture %s (%s): %s%s%s", c.name.c_str(), fx::ReliefClassName(c.relief), reasons.c_str(),
               c.rule && c.relief == fx::ReliefClass::None ? ", rule " : "", c.rule && c.relief == fx::ReliefClass::None ? c.rule : "");
    c.logged = c.seen;
}

void Lighting::log_coverage()
{
    // Per class: textures with the parallax, without it, and why (by reason; a class without relief by its rules).
    struct Row
    {
        uint32_t with = 0, without = 0;
        std::vector<std::pair<std::string, uint32_t>> reasons;
    };
    std::array<Row, static_cast<size_t>(fx::ReliefClass::Character) + 1> rows{};
    for (auto it = parallax_coverage_.begin(); it != parallax_coverage_.end();)
    {
        Coverage &c = it->second;
        if (census_frame_ - c.frame >= 10 * kCoverageFrames)
        {
            it = parallax_coverage_.erase(it); // not drawn for a long while (another location)
            continue;
        }
        if (census_frame_ - c.frame < kCoverageFrames)
        {
            if (c.seen != c.logged)
                log_texture(c);
            Row &row = rows[static_cast<size_t>(c.relief)];
            if (c.status == kCoverageParallax)
                ++row.with;
            else
            {
                ++row.without;
                const std::string reason =
                    c.relief == fx::ReliefClass::None && c.rule ? std::string("rule ") + c.rule
                                                                : ParallaxMissName(static_cast<ParallaxMiss>(c.status));
                const auto found = std::find_if(row.reasons.begin(), row.reasons.end(),
                                                [&](const auto &r) { return r.first == reason; });
                if (found == row.reasons.end())
                    row.reasons.emplace_back(reason, 1);
                else
                    ++found->second;
            }
        }
        c.seen = 0; // the next window's census starts empty
        ++it;
    }
    std::string table;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        Row &row = rows[i];
        if (!row.with && !row.without)
            continue;
        std::sort(row.reasons.begin(), row.reasons.end(), [](const auto &a, const auto &b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        std::string reasons;
        for (const auto &[reason, n] : row.reasons)
            reasons += (reasons.empty() ? "" : ", ") + reason + " " + std::to_string(n);
        table += std::string(table.empty() ? "" : "; ") + fx::ReliefClassName(static_cast<fx::ReliefClass>(i)) + " " +
                 std::to_string(row.with) + " with, " + std::to_string(row.without) + " without" +
                 (reasons.empty() ? "" : " (" + reasons + ")");
    }
    if (table.empty() || table == logged_coverage_)
        return;
    logged_coverage_ = table;
    shim::logf("effects parallax coverage, textures drawn in the last %u frames: %s", kCoverageFrames, table.c_str());
}

void Lighting::submit(const FfDraw &draw)
{
    if (!bgfx::isValid(u_coverage_))
    {
        u_coverage_ = bgfx::createUniform("u_fxCoverage", bgfx::UniformType::Vec4);
        u_ff_ = bgfx::createUniform("u_fxFf", bgfx::UniformType::Vec4);
        u_wet_ = bgfx::createUniform("u_fxWet", bgfx::UniformType::Vec4, 2);
    }
    bgfx::setUniform(u_coverage_, draw.coverage.data());
    if (!draw.lit)
    {
        if (draw.ground)
            shadows_.bind(draw.view, draw.sun_receiver); // the coverage twin of the ground variant
        return;
    }
    // The uniforms and textures of the blocks the draw's program family compiles (FamilyBlocks), with their "off"
    // values where this draw does not use a block its family has.
    const FfBlocks blocks = FamilyBlocks(draw.family);
    bgfx::setUniform(u_ff_, draw.ff.data());
    if (blocks.wet)
        bgfx::setUniform(u_wet_, draw.wet.data(), 2);
    fx_.Apply(draw.light);
    fx_.Apply(draw.points);
    if (bgfx::isValid(draw.normal_map))
        fx_.BindNormalMap(draw.normal_map, draw.normal_map_flags);
    if (!bgfx::isValid(u_gloss_))
        u_gloss_ = bgfx::createUniform("u_fxGloss", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(u_skin_tone_))
        u_skin_tone_ = bgfx::createUniform("u_fxSkinTone", bgfx::UniformType::Vec4, 2);
    bgfx::setUniform(u_skin_tone_, draw.skin_tone.data(), 2);
    bgfx::setUniform(u_gloss_, draw.gloss.data());
    if (bgfx::isValid(draw.material))
        fx_.BindMaterial(draw.material, draw.material_flags);
    if (draw.parallax_variant)
        fx_.SetParallax(draw.parallax);
    if (draw.shadow)
        shadows_.bind(draw.view, draw.sun_receiver);
    else if (draw.shadow_variant)
        shadows_.bind_unshadowed();
    if (blocks.ao)
        bind_ao_volume(draw);
    bind_visibility(draw);
    if (blocks.town_wall)
        bind_town_wall(draw);
    lamps_.bind_view(draw.world_view);
    if (blocks.lamps)
        lamps_.bind(draw.lamps, draw.room_light, draw.aggregate_lamps, draw.aggregate_lamp_overrides);
    if (!bgfx::isValid(s_aggregate_))
        s_aggregate_ = bgfx::createUniform("s_fxAggregate", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(aggregate_open_))
    {
        const std::array<uint8_t,4> zero{};
        aggregate_open_ = bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP, bgfx::copy(zero.data(),sizeof(zero)));
    }
    bgfx::setTexture(fx::kAggregateStage, s_aggregate_, draw.aggregate ? draw.aggregate->texture : aggregate_open_,
                     BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP);
}

std::array<float, 8> Lighting::skin_tone(shim::TexStore &atlas, const DrawSphereSource &draw_source)
{
    if (!atlas.skin_pixels)
        return fx::PackSkinTone({}); // unreadable reference: never turn unknown torso skin into generated relief
    if (!atlas.skin_tone || !atlas.skin_tone->valid)
    {
        const bool first_read = !atlas.skin_tone_read;
        atlas.skin_tone_read = true;
        // The texels under the vertices the head, neck and hand bones carry (the skin weight in the third byte of the
        // bone indices) that face forward in the bind pose (+z: the face, not the hair, a hat or the back of the
        // head), 3 x 3 texels each.
        const DrawVertices vertices = draw_source.vertices();
        const fx::TexturePixels &pixels = *atlas.skin_pixels;
        std::vector<uint8_t> samples;
        // A material can first be drawn by a torso-only range. Retry later ranges until a real reference arrives;
        // cap each draw's work and rotate across large ranges rather than freezing the first empty result forever.
        constexpr uint32_t kVertexBudget = 1024;
        const uint32_t count = std::min(vertices.count, kVertexBudget);
        if (vertices.data && vertices.stride == render::kSkinnedSourceStride && vertices.count)
            for (uint32_t j = 0; j < count; ++j)
            {
                const uint32_t i = (atlas.skin_vertex_cursor + j) % vertices.count;
                const uint8_t *v = vertices.data + static_cast<size_t>(i) * vertices.stride;
                if (v[render::kSkinnedBonesOffset + render::kSkinWeightByte] < 230)
                    continue;
                float n[3], uv[2];
                std::memcpy(n, v + render::kSkinnedNormalOffset, sizeof(n));
                std::memcpy(uv, v + render::kSkinnedUvOffset, sizeof(uv));
                if (n[2] <= 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]))
                    continue;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const float u = uv[0] + static_cast<float>(dx) / static_cast<float>(pixels.width);
                        const float w = uv[1] + static_cast<float>(dy) / static_cast<float>(pixels.height);
                        const int x = static_cast<int>((u - std::floor(u)) * static_cast<float>(pixels.width)) % pixels.width;
                        const int y = static_cast<int>((w - std::floor(w)) * static_cast<float>(pixels.height)) % pixels.height;
                        const uint8_t *t = &pixels.srgb[(static_cast<size_t>(y) * pixels.width + x) * 3];
                        samples.insert(samples.end(), t, t + 3);
                    }
            }
        if (vertices.count)
            atlas.skin_vertex_cursor = (atlas.skin_vertex_cursor + count) % vertices.count;
        if (samples.empty() && !first_read)
            return fx::PackSkinTone({});
        // Small face/hand ranges can contribute fewer than the minimum reference texels. Accumulate only a bounded
        // tail; later actual skin must be able to replace an earlier unusable hat/cloth reference.
        constexpr size_t kSampleBytes = 4096 * 3;
        if (samples.size() > kSampleBytes)
            samples.erase(samples.begin(), samples.end() - kSampleBytes);
        const size_t overflow = atlas.skin_samples.size() + samples.size() > kSampleBytes
                                    ? atlas.skin_samples.size() + samples.size() - kSampleBytes : 0;
        atlas.skin_samples.erase(atlas.skin_samples.begin(), atlas.skin_samples.begin() + overflow);
        atlas.skin_samples.insert(atlas.skin_samples.end(), samples.begin(), samples.end());
        const fx::SkinTone tone = fx::MakeSkinTone(atlas.skin_samples);
        if (tone.valid)
            shim::logf("effects: skin colour of %s: %d texels, chromaticity %.3f %.3f, luminance %.2f..%.2f", atlas.name.c_str(),
                       tone.samples, static_cast<double>(tone.r), static_cast<double>(tone.g), static_cast<double>(tone.lum_lo),
                       static_cast<double>(tone.lum_hi));
        else if (first_read)
            shim::logf("effects: skin colour of %s: none (%d warm face texels%s): its torso relief is masked", atlas.name.c_str(),
                       tone.samples, vertices.data ? "" : ", its vertices unreadable");
        atlas.skin_tone = std::make_shared<const fx::SkinTone>(tone);
        if (tone.valid)
            std::vector<uint8_t>().swap(atlas.skin_samples);
    }
    if (!atlas.skin_tone || !atlas.skin_tone->valid)
        return fx::PackSkinTone({});
    std::array<float, 8> packed = fx::PackSkinTone(*atlas.skin_tone);
    // The lod of the level about kSkinTestTexels across (the atlas's top level is level 0 of the sampler).
    const float top = static_cast<float>(std::max(atlas.width, atlas.height));
    packed[7] = std::max(0.0f, std::log2(top / static_cast<float>(fx::kSkinTestTexels)));
    return packed;
}

void Lighting::submit_ground(const FfDraw &draw)
{
    shadows_.bind(draw.view, draw.sun_receiver);
}

bool Lighting::shadow_volumes(shim::Device &device)
{
    if (!lighting_)
        return false;
    const shim::TexStore *picture = device.back_color ? device.back_color->store.get() : nullptr;
    const shim::TexStore *target = device.rt ? device.rt->store.get() : nullptr;
    if (!target || target != picture)
        return false;
    // The traversal asks for every node and object the camera rejects: the lights are asked once a frame.
    if (shadow_volumes_prepared_)
        return shadow_casters_.volumes() != 0;
    shadow_volumes_prepared_ = true;
    {
        std::array<float, 16> view_proj;
        bx::mtxMul(view_proj.data(), &device.s.view.m[0][0], &device.s.proj.m[0][0]);
        shadow_casters_.set_camera(view_proj, origin_);
    }
    // The same calls the first receiver of the frame makes (prepare()): whichever comes first prepares the maps.
    if (lamps_.enabled())
    {
        const D3DMATRIX &v = device.s.view;
        std::array<float, 3> eye{}; // absolute, like the lamps
        for (int j = 0; j < 3; ++j)
            eye[j] = origin_[j] - (v.m[3][0] * v.m[j][0] + v.m[3][1] * v.m[j][1] + v.m[3][2] * v.m[j][2]);
        lamps_.prepare(device, eye, casters_, shadow_casters_);
    }
    if ((flags_ & fx::kLightingPerPixel) && shadows_.enabled() && !lamps_.interior())
    {
        std::array<float, 16> view;
        memcpy(view.data(), &device.s.view.m[0][0], sizeof(float) * 16);
        shadows_.prepare(device, view, origin_, casters_, shadow_casters_);
    }
    return shadow_casters_.volumes() != 0;
}

bool Lighting::sun_lit(const shim::Device &device) const
{
    if (!device.s.rs[D3DRS_LIGHTING])
        return false;
    for (int stage = 0; stage < 4; ++stage)
        if (device.s.tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE)
            if (const shim::TexStore *store = shim::store_of(device.s.textures[stage]); store && store->lightmap)
                return false;
    return std::any_of(device.lights.begin(), device.lights.end(), [](const auto &entry) {
        return entry.enabled && entry.light.Type == D3DLIGHT_DIRECTIONAL;
    });
}

namespace
{
// A location's AO or visibility volume that arrives where there was none (its bake or cache load ended, possibly after
// the reveal: scene_readiness.hpp kSceneProductBudgetMs) fades in over this instead of popping in.
constexpr std::chrono::milliseconds kVolumeFade{300};
float volume_weight(std::chrono::steady_clock::time_point &since, const void *was, const void *volume)
{
    const auto now = std::chrono::steady_clock::now();
    if (!volume)
        return 0.0f;
    if (!was)
        since = now;
    return std::min(1.0f, std::chrono::duration<float>(now - since).count() / std::chrono::duration<float>(kVolumeFade).count());
}
} // namespace

void Lighting::set_ao_volume(const AoVolumes::Bound *volume)
{
    ao_weight_ = volume_weight(ao_since_, ao_volume_, volume);
    ao_volume_ = volume;
}

void Lighting::set_visibility(const Visibility::Bound *volume)
{
    visibility_weight_ = volume_weight(visibility_since_, visibility_, volume);
    visibility_ = volume;
}

void Lighting::bind_ao_volume(const FfDraw &draw)
{
    if (!bgfx::isValid(u_ao_params_))
    {
        s_ao_volume_ = bgfx::createUniform("s_fxAoVolume", bgfx::UniformType::Sampler);
        u_ao_matrix_ = bgfx::createUniform("u_fxAoVolume", bgfx::UniformType::Mat4);
        u_ao_params_ = bgfx::createUniform("u_fxAoParams", bgfx::UniformType::Vec4);
    }
    // Every lit / baked program reads u_fxAoParams: x = 0 turns the volume off for draws without it. Its 3D sampler
    // still needs a 3D texture on its unit (GL refuses a draw whose sampler types share a unit: an unbound sampler
    // stays on unit 0, the 2D stage 0), so those draws get a 1x1x1 open volume.
    if (!draw.ao || !ao_volume_)
    {
        if (!bgfx::isValid(ao_open_))
        {
            const uint8_t open[4] = {255, 255, 255, 255};
            ao_open_ = bgfx::createTexture3D(1, 1, 1, false, bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_POINT, bgfx::copy(open, 4));
        }
        const float off[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        bgfx::setUniform(u_ao_params_, off);
        bgfx::setTexture(fx::kAoVolumeStage, s_ao_volume_, ao_open_);
        return;
    }
    // A new room (or a rebake with different bounds) can arrive while the camera is still. The volume's
    // transform is part of this cache key too, even when AoVolumes reuses the same Bound object.
    if (draw.world_view != ao_view_ || ao_volume_->world_to_uvw != ao_world_to_uvw_)
    {
        float inverse_view[16];
        bx::mtxInverse(inverse_view, draw.world_view.data());
        bx::mtxMul(ao_view_to_uvw_.data(), inverse_view, ao_volume_->world_to_uvw.data());
        ao_view_ = draw.world_view;
        ao_world_to_uvw_ = ao_volume_->world_to_uvw;
    }
    const fx::AoVolumeLevel &level = fx::kAoVolumeLevel[std::min<uint8_t>(settings_.ao_volume, fx::kAoVolumeLevels)];
    const float params[4] = {level.strength * settings_.ao_gain * ao_weight_, level.contrast, ao_volume_->cell, 1.0f / fx::kAoOpenSurface};
    bgfx::setUniform(u_ao_matrix_, ao_view_to_uvw_.data());
    bgfx::setUniform(u_ao_params_, params);
    bgfx::setTexture(fx::kAoVolumeStage, s_ao_volume_, ao_volume_->texture);
}

void Lighting::bind_visibility(const FfDraw &draw)
{
    if (!bgfx::isValid(u_visibility_params_))
    {
        s_visibility_ = bgfx::createUniform("s_fxVisibility", bgfx::UniformType::Sampler);
        u_visibility_matrix_ = bgfx::createUniform("u_fxVisibility", bgfx::UniformType::Mat4);
        u_visibility_params_ = bgfx::createUniform("u_fxVisibilityParams", bgfx::UniformType::Vec4);
    }
    // Like the AO volume: the 3D sampler needs a 3D texture on its unit whether or not this draw has a volume.
    if (!visibility_)
    {
        if (!bgfx::isValid(visibility_open_))
        {
            const uint8_t open[2] = {255, 255};
            visibility_open_ = bgfx::createTexture3D(1, 1, 1, false, bgfx::TextureFormat::RG8, BGFX_SAMPLER_POINT, bgfx::copy(open, 2));
        }
        const float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bgfx::setUniform(u_visibility_params_, off);
        bgfx::setTexture(fx::kVisibilityStage, s_visibility_, visibility_open_);
        return;
    }
    if (draw.world_view != visibility_view_ || visibility_->world_to_uvw != visibility_world_to_uvw_)
    {
        float inverse_view[16];
        bx::mtxInverse(inverse_view, draw.world_view.data());
        bx::mtxMul(visibility_view_to_uvw_.data(), inverse_view, visibility_->world_to_uvw.data());
        visibility_view_ = draw.world_view;
        visibility_world_to_uvw_ = visibility_->world_to_uvw;
    }
    // x the mode (2: the debug view of the visibility), y the receivers' offset along their normal in metres, z its
    // share while it fades in (kVolumeFade; the debug view shows it whole).
    const float params[4] = {settings_.visibility_debug ? 2.0f : 1.0f, visibility_->offset,
                             settings_.visibility_debug ? 1.0f : visibility_weight_, 0.0f};
    bgfx::setUniform(u_visibility_matrix_, visibility_view_to_uvw_.data());
    bgfx::setUniform(u_visibility_params_, params);
    bgfx::setTexture(fx::kVisibilityStage, s_visibility_, visibility_->texture);
}

void Lighting::bind_town_wall(const FfDraw &draw)
{
    if (!bgfx::isValid(u_town_wall_))
        u_town_wall_ = bgfx::createUniform("u_fxTownWall", bgfx::UniformType::Vec4, 5);
    std::array<std::array<float, 4>, 5> u{};
    const fx::TownLightTone *tone = town_light_.wall_light();
    const fx::TownWallTone *wall = town_light_.wall_tone();
    if (draw.town_wall && tone && wall)
    {
        // Directions into the draw's view space (the device view's rotation; directions are the same absolute or
        // camera-relative).
        auto to_view = [&](const std::array<float, 3> &d, std::array<float, 4> &out) {
            for (int k = 0; k < 3; ++k)
                out[k] = d[0] * draw.view[0 * 4 + k] + d[1] * draw.view[1 * 4 + k] + d[2] * draw.view[2 * 4 + k];
        };
        u[0] = {tone->a, tone->b, tone->c, town_light_.wall_weight()};
        to_view(town_light_.live_sun(), u[1]);
        to_view(town_light_.bake_sun(), u[2]);
        u[2][3] = town_light_.wall_lift();
        for (int i = 0; i < fx::TownWallTone::kSkyBins; ++i)
            u[3 + i / 4][i % 4] = wall->game_sky[i];
    }
    bgfx::setUniform(u_town_wall_, u.data(), 5);
}

bool Lighting::aerial_fog(const D3DMATRIX &view, bool vs_port, std::array<float, 2> &haze)
{
    if (!(flags_ & fx::kLightingAerial) || !sky_)
        return false;
    if (vs_port)
    {
        if (!aerial_port_reported_)
            shim::logf("effects: aerial perspective: draws with a ported vertex shader keep the fixed-function fog");
        aerial_port_reported_ = true;
        return false;
    }
    if (!bgfx::isValid(u_aerial_up_))
        u_aerial_up_ = bgfx::createUniform("u_fxAerialUp", bgfx::UniformType::Vec4);
    // World up in view space is the second row of the row-vector view matrix; the eye's height -t * R^T, in absolute
    // world (the device's view is camera-relative: its eye is at the origin's height).
    const float up[4] = {view.m[1][0], view.m[1][1], view.m[1][2], 0.0f};
    const float eye_height =
        origin_[1] - (view.m[3][0] * view.m[1][0] + view.m[3][1] * view.m[1][1] + view.m[3][2] * view.m[1][2]);
    haze = fx::ComputeAerialFogHaze(eye_height);
    bgfx::setUniform(u_aerial_up_, up);
    ++frame_.aerial;
    return true;
}

void Lighting::end_frame()
{
    // Uploads this frame did not draw with are released (bgfx queues the destroy after the frame's submissions);
    // the rest are reused by the next frame instead of being made again.
    std::erase_if(aggregate_cache_, [&](const auto &cached) { return cached->used_frame != aggregate_frame_; });
    ++aggregate_frame_;
    frame_.flashes = static_cast<uint32_t>(flash_lights_.size()); // the flashes this frame drew with
    last_frame_ = frame_;
    frame_ = {};
    // Every 30 frames, when it changed: the parallax census of the last frame.
    if (++census_frames_ >= 30)
    {
        census_frames_ = 0;
        uint32_t missed = 0;
        for (const uint32_t n : last_frame_.parallax_miss)
            missed += n;
        if ((last_frame_.parallax != logged_parallax_ || last_frame_.parallax_miss != logged_miss_) &&
            (last_frame_.parallax || missed))
        {
            logged_parallax_ = last_frame_.parallax;
            logged_miss_ = last_frame_.parallax_miss;
            std::string without;
            for (size_t i = 0; i < last_frame_.parallax_miss.size(); ++i)
                if (last_frame_.parallax_miss[i])
                    without += std::string(without.empty() ? "" : ", ") + ParallaxMissName(static_cast<ParallaxMiss>(i)) +
                               " " + std::to_string(last_frame_.parallax_miss[i]);
            shim::logf("effects parallax: %u textured draws drew with it, %u without (%s); stage 0's gloss yielded to a "
                       "stage 1 parallax in %u",
                       last_frame_.parallax, missed, without.empty() ? "none" : without.c_str(), last_frame_.gloss_yields);
        }
    }
    ++census_frame_;
    if (census_frame_ % kCoverageFrames == 0)
        log_coverage();
    town_light_.end_frame();
    shadows_.end_frame();
    normal_maps_.end_frame();
    lamps_.end_frame();
    shadow_casters_.end_frame(); // after the lights: they read their volumes' counts in their own end_frame()
    shadow_volumes_prepared_ = false;
    if (!dynamic_lights_this_frame_)
        dynamic_lights_.clear(); // no location reported them: none exist
    dynamic_lights_this_frame_ = false;
    // Flashes age with the presented frames in real time (a 0.2 s flash is the same at 30 and 60 fps), but by at most
    // a 30 fps frame per frame: below that rate a flash is slowed down, never skipped or shown for a single frame.
    constexpr float kMaxFlashStep = 1.0f / 30.0f;
    const auto now = std::chrono::steady_clock::now();
    const float dt = flash_clock_.time_since_epoch().count() == 0
                         ? 0.0f
                         : std::min(std::chrono::duration<float>(now - flash_clock_).count(), kMaxFlashStep);
    flash_clock_ = now;
    if (flash_cap_ > 0 && (flashes_.Count() > 0 || !flash_lights_.empty()))
    {
        flashes_.Update(dt, origin_, flash_cap_);
        flash_lights_.clear();
        flashes_.AppendTo(flash_lights_, settings_.light_falloff);
        device_lights_origin_valid_ = false;
    }
}

void Lighting::shutdown()
{
    aggregate_cache_.clear();
    if (bgfx::isValid(aggregate_open_)) bgfx::destroy(aggregate_open_);
    if (bgfx::isValid(s_aggregate_)) bgfx::destroy(s_aggregate_);
    aggregate_open_ = BGFX_INVALID_HANDLE;
    s_aggregate_ = BGFX_INVALID_HANDLE;
    probe_.shutdown();
    fx_.Shutdown();
    shadows_.shutdown();
    town_light_.shutdown();
    lamps_.shutdown();
    casters_.shutdown();
    if (bgfx::isValid(ao_open_))
        bgfx::destroy(ao_open_);
    if (bgfx::isValid(visibility_open_))
        bgfx::destroy(visibility_open_);
    visibility_open_ = BGFX_INVALID_HANDLE;
    ao_open_ = BGFX_INVALID_HANDLE;
    for (bgfx::UniformHandle *u : {&u_coverage_, &u_ff_, &u_wet_, &u_gloss_, &u_skin_tone_, &u_aerial_up_, &s_ao_volume_, &u_ao_matrix_, &u_ao_params_, &s_visibility_,
                                   &u_visibility_matrix_, &u_visibility_params_, &u_town_wall_})
    {
        if (bgfx::isValid(*u))
            bgfx::destroy(*u);
        *u = BGFX_INVALID_HANDLE;
    }
}

} // namespace storm::effects
