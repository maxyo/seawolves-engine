#include "effects/town_light.hpp"

#include "effects/casters.hpp"
#include "effects/shadows.hpp"
#include "shim/shim.hpp"
#include "shim/texture_fades.hpp"
#include "storm/render/frame_owner.hpp"
#include "storm/fx/hash.hpp"
#include "texture/core/formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace storm::effects
{

namespace
{
constexpr float kTexelMetres = 0.3f; // the footprint the bake's sun is read over on a texel by the fit's rays (fx::SunVisibility)
constexpr bgfx::ViewId kPlaceView = render::kPreGameViewBegin + 4;   // the place's sun map
constexpr bgfx::ViewId kComposeView = render::kPreGameViewBegin + 5; // one lightmap a frame
constexpr size_t kFitSamples = 1500;
// A lightmap set's section of the place's cache file (effects/place.hpp PlaceStore), keyed by the place and the set
// (its lightmapped triangles and its lightmaps' texels): the fit of the direction the set was baked with and, when
// that fit is trusted, the lightmaps' tones and the walls'. Bump kSetVersion on any change of what a set's bake makes.
// 2: the bake's sun on the texels is no longer kept (the GPU reads it from the place map, fx/sun_relight.hpp).
constexpr uint32_t kSetVersion = 2;
constexpr size_t kSetsKept = 8; // the sets of a town's hours (morning, day1..4, evening) and spare

// A section written in order, or read back in order.
struct Section
{
    std::vector<uint8_t> bytes;
    size_t cursor = 0;
    bool take(void *out, size_t size)
    {
        if (size > bytes.size() - cursor)
            return false;
        std::memcpy(out, bytes.data() + cursor, size);
        cursor += size;
        return true;
    }
    template <class T> bool take(T &value) { return take(&value, sizeof(value)); }
    void put(const void *in, size_t size)
    {
        bytes.insert(bytes.end(), static_cast<const uint8_t *>(in), static_cast<const uint8_t *>(in) + size);
    }
    template <class T> void put(const T &value) { put(&value, sizeof(value)); }
};

// Field by field (no padding goes to the file).
void Put(Section &out, const fx::SunFit &fit)
{
    out.put(fit.dir.data(), sizeof(fit.dir));
    out.put(fit.r);
    out.put(fit.shadowed);
    out.put(static_cast<uint8_t>(fit.valid ? 1 : 0));
}
bool Take(Section &in, fx::SunFit &fit)
{
    uint8_t valid = 0;
    if (!in.take(fit.dir.data(), sizeof(fit.dir)) || !in.take(fit.r) || !in.take(fit.shadowed) || !in.take(valid))
        return false;
    fit.valid = valid != 0;
    return true;
}
void Put(Section &out, const fx::TownLightTone &tone)
{
    out.put(tone.a);
    out.put(tone.b);
    out.put(tone.c);
    out.put(tone.r2);
    out.put(tone.edges);
    out.put(static_cast<uint8_t>(tone.valid ? 1 : 0));
    out.put(tone.game_sky, sizeof(tone.game_sky));
}
bool Take(Section &in, fx::TownLightTone &tone)
{
    uint8_t valid = 0;
    if (!in.take(tone.a) || !in.take(tone.b) || !in.take(tone.c) || !in.take(tone.r2) || !in.take(tone.edges) ||
        !in.take(valid) || !in.take(tone.game_sky, sizeof(tone.game_sky)))
        return false;
    tone.valid = valid != 0;
    return true;
}
void Put(Section &out, const fx::TownWallTone &wall)
{
    out.put(wall.game_sky, sizeof(wall.game_sky));
    out.put(wall.relative, sizeof(wall.relative));
    out.put(wall.count, sizeof(wall.count));
    out.put(wall.vertices);
    out.put(wall.shaded);
    out.put(static_cast<uint8_t>(wall.valid ? 1 : 0));
}
bool Take(Section &in, fx::TownWallTone &wall)
{
    uint8_t valid = 0;
    if (!in.take(wall.game_sky, sizeof(wall.game_sky)) || !in.take(wall.relative, sizeof(wall.relative)) ||
        !in.take(wall.count, sizeof(wall.count)) || !in.take(wall.vertices) || !in.take(wall.shaded) || !in.take(valid))
        return false;
    wall.valid = valid != 0;
    return true;
}

double Since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

float Angle(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
    return std::acos(std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f)) * 57.29578f;
}

unsigned Threads()
{
    return fx::BakeThreads();
}

} // namespace

void TownLight::set_lightmaps(std::string_view name, std::span<const std::array<float, 3>> positions,
                              std::span<const std::array<float, 2>> uvs, std::span<const uint32_t> indices,
                              std::span<const uint32_t> triangle_lightmap, std::span<const render::EffectsLightmap> lightmaps)
{
    relight_.ClearMaps();
    maps_.clear();
    place_sun_ = {0.0f, 0.0f, 0.0f};
    place_job_.reset();
    ready_ = false;
    wall_ = {};
    wall_ready_ = false;
    ratio_missed_logged_ = false;
    name_ = std::string(name);
    auto lightmapped = std::make_shared<Lightmapped>();
    lightmapped->positions.assign(positions.begin(), positions.end());
    lightmapped->uvs.assign(uvs.begin(), uvs.end());
    lightmapped->indices.assign(indices.begin(), indices.end());
    lightmapped->triangle_lightmap.assign(triangle_lightmap.begin(), triangle_lightmap.end());
    lightmapped->lightmaps.assign(lightmaps.begin(), lightmaps.end());
    lightmapped_ = std::move(lightmapped);
    want_place_ = !indices.empty();
    if (indices.empty())
    {
        if (!name_.empty())
            shim::logf("effects: town light of %s: no lightmapped geometry reported: none here", name_.c_str());
        return;
    }
    if (!enabled_)
        shim::logf("effects: town light of %s: not baked while fx_town_light is off (the preset or the option): baked "
                   "when it is switched on", name_.c_str());
}

bool TownLight::preparing() const
{
    if (!enabled_ || lightmapped_->indices.empty())
        return false;
    if (place_job_ || (want_place_ && place_ && sun_present_ && maps_.empty()))
        return true;
    return !maps_.empty() && !ready_ && fx::SunRelight::Supported();
}

void TownLight::set_place(std::shared_ptr<const PlaceReady> place)
{
    if (place && (place->place->interior || place->place->name != name_ || !place->volume))
        place.reset(); // not this town's (the lightmaps of the next place come first), or no town
    if (place == place_)
        return;
    // Another place, or this one's probes made again (another grid, a lost device): its set is made again from them.
    place_ = std::move(place);
    relight_.ClearMaps();
    maps_.clear();
    place_sun_ = {0.0f, 0.0f, 0.0f};
    place_job_.reset();
    ready_ = false;
    wall_ = {};
    wall_ready_ = false;
    want_place_ = !lightmapped_->indices.empty();
}

void TownLight::set_enabled(bool enabled)
{
    if (enabled == enabled_)
        return;
    enabled_ = enabled;
    if (!enabled_)
    {
        // Nothing shows our lightmaps now: the bakes still running are cancelled (what is done stays for the next
        // switch on).
        if (place_job_)
        {
            place_job_.reset();
            want_place_ = true;
        }
    }
}

void TownLight::start_place()
{
    want_place_ = false;
    auto job = std::make_shared<PlaceJob>();
    // The game's lightmaps' textures, looked up here (main thread): the draws are matched by them.
    std::vector<std::shared_ptr<const shim::TexStore>> stores;
    for (const render::EffectsLightmap &m : lightmapped_->lightmaps)
        stores.push_back(shim::shared_store_of(m.texture));
    place_job_.start("town light of " + name_, job,
                     [job, stores = std::move(stores), lightmapped = lightmapped_, place = place_](fx::BakeStage stage) {
        const auto started = std::chrono::steady_clock::now();
        const Lightmapped &lm = *lightmapped;
        const Place &geometry = *place->place;
        const fx::VisibilityVolume &volume = *place->volume;
        if (stage == fx::BakeStage::Load)
        {
            // The lightmaps' texels and the sky on them (from the place's probes), and the set's key.
            fx::Hasher key;
            key.Value(kSetVersion).Value(place->store->key());
            key.Span(std::span(lm.positions)).Span(std::span(lm.uvs)).Span(std::span(lm.indices)).Span(std::span(lm.triangle_lightmap));
            for (size_t m = 0; m < lm.lightmaps.size(); ++m)
            {
                const render::EffectsLightmap &map_in = lm.lightmaps[m];
                key.Value(map_in.width).Value(map_in.height).Span(std::span<const char>(map_in.format)).Span(std::span(map_in.data));
                uint32_t code; // the .tx format tag is the D3DFORMAT FourCC
                std::memcpy(&code, map_in.format.data(), sizeof(code));
                const char kind = map_in.format[3];
                Map map;
                map.store = stores[m];
                map.serial = stores[m] ? stores[m]->serial : 0;
                map.game.resize(static_cast<size_t>(map_in.width) * map_in.height * 4);
                if (!stores[m] || map_in.format[0] != 'D' || !(kind == '1' || kind == '3' || kind == '5') || map_in.width < 4 ||
                    map_in.height < 4 || map_in.data.size() < texture::d3d_level_bytes(code, map_in.width, map_in.height) ||
                    !texture::decode_rgba8(code, map_in.data.data(), map_in.width, map_in.height, map.game.data()))
                {
                    job->how += " (a lightmap that is no DXT1/3/5 of its size, or has no texture, keeps the game's)";
                    continue;
                }
                map.texels = std::make_shared<const fx::LightmapTexels>(fx::RasterizeLightmap(
                    lm.positions, lm.uvs, lm.indices, lm.triangle_lightmap, static_cast<uint32_t>(m),
                    static_cast<int>(map_in.width), static_cast<int>(map_in.height)));
                if (map.texels->texel.empty())
                    continue;
                map.sky = fx::TexelSky(*map.texels, volume, *place->near_sky);
                job->maps.push_back(std::move(map));
            }
            job->set_key = key.Result();
            Section cached{place->store->find(fx::PlaceSection::LightmapSet, job->set_key)};
            if (cached.bytes.empty())
                return false; // a new set: fitted on the bake thread
            bool whole = Take(cached, job->bake);
            if (whole && job->bake.valid)
            {
                for (Map &map : job->maps)
                    whole = whole && Take(cached, map.tone);
                whole = whole && Take(cached, job->wall) && Take(cached, job->wall_light);
            }
            if (!whole || cached.cursor != cached.bytes.size())
            {
                shim::logf("ERROR: effects: town light of %s: its cached lightmap set does not fit its lightmaps: made again",
                           geometry.name.c_str());
                return false;
            }
            char how[120];
            std::snprintf(how, sizeof(how), "its lightmap set from the cache in %.0f ms", Since(started));
            job->how = how + job->how;
            job->state = 1;
            return true;
        }
        const unsigned threads = Threads();
        double rays_ms = 0.0;
        const std::shared_ptr<const fx::Occluders> rays = place->rays->get(&rays_ms);
        const fx::Occluders &occluders = *rays;
        // What the bake's sun is fitted to: the ground's texels open to the sky, with the game's luminance over its
        // neighbourhood's (the paint's grass and sand are broad, the shadows' edges sharp).
        std::vector<fx::SunFitSample> samples;
        for (const Map &map : job->maps)
        {
            const int w = map.texels->width, h = map.texels->height, r = 6;
            std::vector<float> lum(static_cast<size_t>(w) * h), blur(lum.size()), row(lum.size());
            for (size_t i = 0; i < lum.size(); ++i)
                lum[i] = (0.299f * map.game[i * 4] + 0.587f * map.game[i * 4 + 1] + 0.114f * map.game[i * 4 + 2]) / 255.0f;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float sum = 0.0f;
                    for (int d = -r; d <= r; ++d)
                        sum += lum[static_cast<size_t>(y) * w + std::clamp(x + d, 0, w - 1)];
                    row[static_cast<size_t>(y) * w + x] = sum / (2 * r + 1);
                }
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float sum = 0.0f;
                    for (int d = -r; d <= r; ++d)
                        sum += row[static_cast<size_t>(std::clamp(y + d, 0, h - 1)) * w + x];
                    blur[static_cast<size_t>(y) * w + x] = sum / (2 * r + 1);
                }
            for (size_t i = 0; i < map.texels->texel.size(); i += 5)
                if (map.texels->normal[i][1] > 0.7f && map.sky[i] > 128)
                {
                    const uint32_t at = map.texels->texel[i];
                    samples.push_back({map.texels->position[i], std::min(2.0f, lum[at] / std::max(blur[at], 0.02f))});
                }
        }
        if (samples.size() > kFitSamples)
        {
            std::vector<fx::SunFitSample> spread;
            for (size_t i = 0; i < kFitSamples; ++i)
                spread.push_back(samples[i * samples.size() / kFitSamples]);
            samples.swap(spread);
        }
        auto t = std::chrono::steady_clock::now();
        job->bake = fx::FitBakedSun(occluders, samples, 200.0f, threads);
        const double fit_ms = Since(t);
        double sun_ms = 0.0, wall_ms = 0.0;
        Section section;
        Put(section, job->bake);
        if (job->bake.valid)
        {
            t = std::chrono::steady_clock::now();
            const Map *first = nullptr;
            for (Map &map : job->maps)
            {
                const std::vector<uint8_t> baked = fx::SunVisibility(occluders, *map.texels, job->bake.dir, kTexelMetres, 4, threads);
                map.tone = fx::CalibrateTownLight(*map.texels, map.game, baked, map.sky);
                Put(section, map.tone);
                if (!first && map.tone.valid)
                    first = &map;
            }
            sun_ms = Since(t);
            if (first && geometry.colors.size() == geometry.positions.size())
            {
                // The walls are the still geometry's triangles that are not under a lightmap (those are relit as
                // texels): matched by their centroids, to the centimetre.
                t = std::chrono::steady_clock::now();
                auto centroid = [](const fx::Vec3 &a, const fx::Vec3 &b, const fx::Vec3 &c) {
                    int64_t q[3];
                    for (int k = 0; k < 3; ++k)
                        q[k] = std::llround((a[k] + b[k] + c[k]) * (100.0f / 3.0f));
                    return (static_cast<uint64_t>(q[0]) * 73856093u) ^ (static_cast<uint64_t>(q[1]) * 19349663u) ^
                           (static_cast<uint64_t>(q[2]) * 83492791u);
                };
                std::unordered_set<uint64_t> lit;
                for (size_t i = 0; i + 2 < lm.indices.size(); i += 3)
                    lit.insert(centroid(lm.positions[lm.indices[i]], lm.positions[lm.indices[i + 1]], lm.positions[lm.indices[i + 2]]));
                const std::vector<fx::Vec3> &p = geometry.positions;
                const std::vector<uint32_t> &idx = geometry.indices;
                std::vector<uint8_t> skip(idx.size() / 3, 0);
                for (size_t i = 0; i < skip.size(); ++i)
                    skip[i] = lit.count(centroid(p[idx[3 * i]], p[idx[3 * i + 1]], p[idx[3 * i + 2]])) ? 1 : 0;
                job->wall_light = first->tone;
                job->wall = fx::CalibrateTownWalls(p, geometry.colors, idx, skip, volume, first->tone, job->bake.dir, occluders, threads);
                wall_ms = Since(t);
            }
            Put(section, job->wall);
            Put(section, job->wall_light);
        }
        const std::string unsaved = place->store->put(fx::PlaceSection::LightmapSet, job->set_key, std::move(section.bytes), kSetsKept);
        char how[300];
        std::snprintf(how, sizeof(how),
                      "its lightmap set made in %.0f ms on up to %u threads (ray structure %.0f ms, fit %.0f ms, its sun %.0f "
                      "ms, walls %.0f ms)%s%s%s",
                      Since(started), threads, rays_ms, fit_ms, sun_ms, wall_ms, unsaved.empty() ? "" : " (",
                      unsaved.c_str(), unsaved.empty() ? "" : ")");
        job->how = how + job->how;
        job->state = 1;
        return true;
    });
}

void TownLight::update(const std::array<float, 3> &sun, bool present)
{
    sun_present_ = present;
    live_sun_ = sun;
    if (!enabled_)
    {
        fade_walls();
        fade_ratio();
        return; // no bake runs (set_enabled)
    }
    // The set's job: once the place's probes are there and the sun is up (nothing is fitted at night: the game's
    // night lightmaps hold no sun to find).
    if (want_place_ && place_ && present && !place_job_ && maps_.empty())
        start_place();
    if (place_job_ && place_job_->state.load() == 1)
    {
        PlaceJob &job = *place_job_;
        maps_ = std::move(job.maps);
        size_t texels = 0, valid = 0;
        for (const Map &m : maps_)
        {
            texels += m.texels->texel.size();
            valid += job.bake.valid && m.tone.valid ? 1 : 0;
        }
        const fx::SunFit &bake = job.bake;
        bake_dir_ = bake.dir;
        shim::logf("effects: town light of %s: %zu lightmaps (%zu texels, %zu relit), the game's baked with the sun at "
                   "(%.2f, %.2f, %.2f), %.0f° up (r %.2f), %s",
                   name_.c_str(), maps_.size(), texels, valid, bake.dir[0], bake.dir[1], bake.dir[2],
                   std::asin(std::clamp(bake.dir[1], -1.0f, 1.0f)) * 57.29578f, bake.r, job.how.c_str());
        if (!bake.valid)
        {
            // Relit with a sun that is not the game's, the game's painted shadows would stay and ours come on top.
            shim::logf("effects: town light of %s: NOT relit: the game's shadows agree with no sun direction well enough (r "
                       "%.2f under %.2f): its lightmaps and walls stay the game's, no sun is baked for them",
                       name_.c_str(), bake.r, fx::kSunFitMinR);
            maps_.clear();
        }
        for (const Map &m : maps_)
            shim::logf("effects: town light of %s: a lightmap of %dx%d lit %.3f + %.3f sun + %.3f sky (R² %.2f, sun from %u "
                       "shadow edges), the game's sky for ours 0.06..0.94: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f%s",
                       name_.c_str(), m.texels->width, m.texels->height, m.tone.a, m.tone.b, m.tone.c, m.tone.r2, m.tone.edges,
                       m.tone.game_sky[0], m.tone.game_sky[1], m.tone.game_sky[2], m.tone.game_sky[3], m.tone.game_sky[4],
                       m.tone.game_sky[5], m.tone.game_sky[6], m.tone.game_sky[7],
                       m.tone.valid ? "" : ": not explained, keeps the game's");
        // A lightmap whose tone the game's does not explain keeps the game's.
        std::erase_if(maps_, [](const Map &m) { return !m.tone.valid; });
        if (!maps_.empty())
        {
            wall_ = job.wall;
            wall_light_ = job.wall_light;
            wall_ready_ = true;
            const fx::TownWallTone &w = wall_;
            shim::logf("effects: town light of %s: walls %s: %u vertices, %u in the bake's shade; their light per bin of our "
                       "sky over the open ones: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f (counts %u %u %u %u %u %u %u %u), the "
                       "game's sky %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
                       name_.c_str(), w.valid ? "calibrated" : "NOT calibrated (too few shaded open vertices): not relit",
                       w.vertices, w.shaded, w.relative[0], w.relative[1], w.relative[2], w.relative[3], w.relative[4],
                       w.relative[5], w.relative[6], w.relative[7], w.count[0], w.count[1], w.count[2], w.count[3], w.count[4],
                       w.count[5], w.count[6], w.count[7], w.game_sky[0], w.game_sky[1], w.game_sky[2], w.game_sky[3],
                       w.game_sky[4], w.game_sky[5], w.game_sky[6], w.game_sky[7]);
        }
        place_job_.reset();
    }
    fade_walls();
    fade_ratio();
}

float TownLight::wall_lift() const
{
    return fx::RelightLift(Angle(live_sun_, {bake_dir_[0], bake_dir_[1], bake_dir_[2]}));
}

void TownLight::fade_walls()
{
    // The walls' relight fades in as our lightmaps do (shim texture_fade.hpp): it comes with them, possibly after the
    // location was revealed.
    const bool walls = walls_active();
    const uint64_t now = shim::texture_fades().now();
    if (walls && !walls_were_active_)
    {
        walls_since_ = now;
        shim::logf("effects: town light of %s: the walls' relight fades in over %llu ms", name_.c_str(),
                   static_cast<unsigned long long>(shim::kTextureFadeMs));
    }
    walls_were_active_ = walls;
    wall_weight_ = walls ? 1.0f - shim::TextureFade::Share(now - walls_since_) : 0.0f;
}

void TownLight::fade_ratio()
{
    // The ratio fades in when it comes (shim texture_fade.hpp's time: after the location was revealed too, no pop of
    // the shadows) and out when the town light is switched off; it goes at once with the place or its lightmap set.
    const bool shown = active();
    const uint64_t now = shim::texture_fades().now();
    if (shown != ratio_shown_)
    {
        ratio_shown_ = shown;
        ratio_from_ = maps_.empty() ? 0.0f : ratio_weight_;
        ratio_since_ = now;
        if (ratio_from_ > 0.0f || shown)
            shim::logf("effects: town light of %s: the light ratio fades %s over %llu ms", name_.c_str(), shown ? "in" : "out",
                       static_cast<unsigned long long>(shim::kTextureFadeMs));
    }
    const float share = shim::TextureFade::Share(now - ratio_since_); // 1 -> 0 over the fade
    ratio_weight_ = shown ? 1.0f - share * (1.0f - ratio_from_) : ratio_from_ * share;
    if (maps_.empty())
        ratio_weight_ = 0.0f; // its maps are gone
}

void TownLight::RetireRatio(bgfx::TextureHandle texture)
{
    shim::texture_fades().retire(texture);
}

void TownLight::upload_map(Map &map)
{
    // The texels' static inputs as full grids (the gutter takes its covered texel's): what the compose pass reads.
    const fx::LightmapTexels &texels = *map.texels;
    const size_t count = static_cast<size_t>(texels.width) * static_cast<size_t>(texels.height);
    std::vector<float> position(count * 4, 0.0f);
    std::vector<uint8_t> normal_sky(count * 4, 0);
    for (size_t i = 0; i < texels.texel.size(); ++i)
    {
        const size_t at = static_cast<size_t>(texels.texel[i]) * 4;
        for (int k = 0; k < 3; ++k)
        {
            position[at + k] = texels.position[i][k];
            normal_sky[at + k] = static_cast<uint8_t>(std::lround(std::clamp(texels.normal[i][k] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
        }
        position[at + 3] = 1.0f;
        normal_sky[at + 3] = map.sky[i];
    }
    for (size_t g = 0; g + 1 < texels.gutter.size(); g += 2)
    {
        const size_t to = static_cast<size_t>(texels.gutter[g]) * 4, from = static_cast<size_t>(texels.gutter[g + 1]) * 4;
        std::copy_n(&position[from], 4, &position[to]);
        std::copy_n(&normal_sky[from], 4, &normal_sky[to]);
    }
    map.texel_metres = TexelMetres(texels);
    // The ratio map's texels about fx::SunRelight::kRatioTexelMetres apart, within the device's largest texture and no
    // finer than the game's lightmap texture itself.
    const int largest = std::min(static_cast<int>(bgfx::getCaps()->limits.maxTextureSize),
                                 [&] {
                                     const auto store = map.store.lock();
                                     return store ? static_cast<int>(std::max(store->width, store->height)) : texels.width;
                                 }());
    const std::array<int, 2> size = fx::SunRelight::RatioSize(texels.width, texels.height, map.texel_metres, largest);
    map.ratio_metres = map.texel_metres * static_cast<float>(texels.width) / static_cast<float>(size[0]);
    map.relight = relight_.AddMap({texels.width, texels.height, position, normal_sky, map.game}, size[0], size[1]);
    if (map.relight != fx::SunRelight::kNoMap)
        shim::logf("effects: town light of %s: a %dx%d lightmap (texels %.2f m apart) takes a %dx%d light ratio map (%.2f m), "
                   "%.1f MB of GPU memory for the town's relight (and %.1f MB "
                   "uploaded for a compose, let go after it)",
                   name_.c_str(), texels.width, texels.height, map.texel_metres, size[0], size[1], map.ratio_metres,
                   static_cast<double>(relight_.Bytes()) / (1024.0 * 1024.0),
                   static_cast<double>(relight_.PassBytes()) / (1024.0 * 1024.0));
    map.compose_due = true;
}

float TownLight::TexelMetres(const fx::LightmapTexels &texels)
{
    // The median distance between covered texels side by side in a row: a lightmap spreads its texels unevenly
    // (a town's 512 x 512 over a few hundred metres: 0.3-1 m), the median ignores the seams between its charts.
    std::vector<int32_t> at(static_cast<size_t>(texels.width) * texels.height, -1);
    for (size_t i = 0; i < texels.texel.size(); ++i)
        at[texels.texel[i]] = static_cast<int32_t>(i);
    std::vector<float> step;
    for (size_t i = 0; i < texels.texel.size(); i += 3)
    {
        const uint32_t t = texels.texel[i];
        if (static_cast<int>(t % texels.width) + 1 >= texels.width || at[t + 1] < 0)
            continue;
        const fx::Vec3 &a = texels.position[i], &b = texels.position[static_cast<size_t>(at[t + 1])];
        step.push_back(std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])));
    }
    if (step.empty())
        return kTexelMetres; // scattered texels, none beside another: the fit's footprint
    std::nth_element(step.begin(), step.begin() + static_cast<std::ptrdiff_t>(step.size() / 2), step.end());
    return std::clamp(step[step.size() / 2], 0.05f, 4.0f);
}

void TownLight::compose_all()
{
    for (Map &map : maps_)
        map.compose_due = true;
}

void TownLight::render(shim::Device &device, SunShadows &shadows, Casters &casters)
{
    if (!enabled_ || maps_.empty())
        return;
    // Without a sun (night, or the first frames before the weather) the maps are composed for the night: waiting for a
    // sunrise would keep a sunless place behind the readiness fader. A sun that arrives later composes them again.
    if (!fx::SunRelight::Supported())
    {
        if (!unsupported_reported_)
            shim::logf("ERROR: effects: town light of %s: this device has no D16 array / RGBA32F texture / RGBA8 target: the "
                       "town's lightmaps are not relit (the game's stay)", name_.c_str());
        unsupported_reported_ = true;
        return;
    }
    for (Map &map : maps_)
        if (map.relight == fx::SunRelight::kNoMap)
        {
            upload_map(map);
            if (map.relight == fx::SunRelight::kNoMap)
            {
                shim::logf("ERROR: effects: town light of %s: a lightmap of %dx%d could not be set up for the relight: "
                           "the game's stays", name_.c_str(), map.texels->width, map.texels->height);
                map.compose_due = false;
            }
        }
    const bool up = sun_present_ && live_sun_[1] > 0.0f;
    const uint64_t cut = shadows.cut_signature();
    if (cut != place_cut_signature_ && relight_.PlaceDrawn())
    {
        // The still geometry casts otherwise now (a cut-out texture): both suns are read again.
        relight_.InvalidateBakes();
        place_sun_ = {0.0f, 0.0f, 0.0f};
    }
    const auto draw_place = [&](const std::array<float, 3> &sun) {
        const fx::SunPlace place = fx::ComputeSunPlace({sun[0], sun[1], sun[2]}, lightmapped_->positions, fx::kSunRayReach,
                                                       fx::SunRelight::kPlaceSize);
        relight_.BeginPlace(kPlaceView, place, {0.0f, 0.0f, 0.0f});
        shadows.draw_tiles(device, casters, kPlaceView, {0.0f, 0.0f, 0.0f},
                           [&place](const fx::Vec3 &center, float radius) { return place.Reaches(center, radius); });
        place_sun_ = sun;
        place_cut_signature_ = shadows.cut_signature();
    };
    // The bake's sun on each lightmap's texels, once: the place map along the bake's direction, one lightmap a frame.
    // Nothing is composed before (the compose divides by it).
    const auto bake_due = std::find_if(maps_.begin(), maps_.end(), [this](const Map &m) {
        return m.relight != fx::SunRelight::kNoMap && !relight_.BakeReady(m.relight);
    });
    if (bake_due != maps_.end())
    {
        const std::array<float, 3> bake{bake_dir_[0], bake_dir_[1], bake_dir_[2]};
        if (!place_is_bake_ || place_sun_ != bake || !relight_.PlaceDrawn())
        {
            draw_place(bake);
            place_is_bake_ = true;
            ++sun_counters_.bake_draws;
        }
        if (relight_.BakeVisibility(kComposeView, bake_due->relight, 0.5f * bake_due->ratio_metres))
            compose_all();
        return;
    }
    // The place's sun map: when the sun moved a step, or what it was drawn with changed.
    const bool place_void = place_sun_ == std::array<float, 3>{0.0f, 0.0f, 0.0f} || !relight_.PlaceDrawn();
    if (up && (place_void || place_is_bake_ || Angle(live_sun_, place_sun_) > fx::kSunStepDegrees))
    {
        draw_place(live_sun_);
        place_is_bake_ = false;
        ++sun_counters_.place_draws;
        compose_all();
    }
    if (up == night_)
    {
        night_ = !up;
        compose_all();
    }
    // One ratio map a frame (one view): a town has one to four. The taps spread over the texel (its sun is the share
    // of it the sun reaches: no stair steps of whole texels along a shadow's edge).
    // A lightmap the game has freed (before it reports the lightmaps gone) is drawn no more: no ratio for it.
    for (Map &map : maps_)
        if (map.compose_due && map.relight != fx::SunRelight::kNoMap && !map.store.expired())
        {
            fx::RelightTone tone{map.tone.a, map.tone.b, map.tone.c, {}, {bake_dir_[0], bake_dir_[1], bake_dir_[2]},
                                 fx::RelightLift(Angle(live_sun_, {bake_dir_[0], bake_dir_[1], bake_dir_[2]}))};
            std::copy_n(map.tone.game_sky, fx::TownLightTone::kSkyBins, tone.game_sky);
            if (!relight_.Compose(kComposeView, map.relight, tone, up, 0.5f * map.ratio_metres, debug_))
                break; // its bake's sun is not read yet
            map.compose_due = false;
            ++sun_counters_.composes;
            break;
        }
    // The ratios apply once every lightmap's has been composed (the maps exist from the next frame on).
    if (!ready_ && std::none_of(maps_.begin(), maps_.end(), [](const Map &m) { return m.compose_due; }))
    {
        ready_ = true;
        shim::logf("effects: town light of %s: %zu lightmaps relit for the sun at (%.3f, %.3f, %.3f), %.1f° up", name_.c_str(),
                   maps_.size(), live_sun_[0], live_sun_[1], live_sun_[2],
                   std::asin(std::clamp(live_sun_[1], -1.0f, 1.0f)) * 57.29578f);
    }
}

void TownLight::set_debug(bool debug)
{
    if (debug == debug_)
        return;
    debug_ = debug;
    compose_all();
}

TownLight::Ratio TownLight::ratio(const shim::TexStore *store)
{
    ++counts_[0];
    if (!store || ratio_weight_ <= 0.0f || !ready_)
        return {};
    for (const Map &map : maps_)
        if (map.serial == store->serial)
        {
            const bgfx::TextureHandle texture = relight_.Texture(map.relight);
            if (!bgfx::isValid(texture))
                return {};
            ++counts_[1];
            return {texture, ratio_weight_, debug_};
        }
    return {};
}

void TownLight::ratio_missed(const shim::TexStore *store, const char *why)
{
    --counts_[1];
    ++counts_[2];
    if (ratio_missed_logged_)
        return;
    ratio_missed_logged_ = true;
    shim::logf("ERROR: effects: town light of %s: a draw of the lightmap %s cannot take the light ratio (%s): drawn with "
               "the game's lightmap as it is (said once per place; the profile's town_ratio_missed counts them)",
               name_.c_str(), store ? store->name.c_str() : "?", why);
}

float TownLight::sun_shade(const shim::TexStore *store, float unmeasured) const
{
    if (!active() || !store)
        return unmeasured;
    for (const Map &map : maps_)
        if (map.serial == store->serial && bgfx::isValid(relight_.Texture(map.relight)) && ratio_weight_ > 0.0f)
        {
            const float sky = map.tone.a + map.tone.c;
            return sky / std::max(sky + map.tone.b * std::max(live_sun_[1], 0.0f), 1e-3f);
        }
    return unmeasured;
}

void TownLight::end_frame()
{
    last_counts_ = counts_;
    counts_ = {};
}

void TownLight::shutdown()
{
    relight_.Shutdown();
    maps_.clear();
    wall_ready_ = false;
    place_job_.reset();
    ready_ = false;
    want_place_ = !lightmapped_->indices.empty();
}

} // namespace storm::effects
