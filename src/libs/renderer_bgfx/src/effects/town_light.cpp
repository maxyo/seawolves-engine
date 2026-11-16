#include "effects/town_light.hpp"

#include "effects/occluder_log.hpp"
#include "effects/casters.hpp"
#include "effects/shadows.hpp"

#include "shim/shim.hpp"
#include "storm/render/frame_owner.hpp"
#include "texture/core/cache.hpp"
#include "texture/core/formats.hpp"
#include "texture/core/service.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace storm::effects
{

namespace
{
constexpr float kBounceStepDegrees = 2.0f; // the sun's bounce is baked again when the hour's sun moved this far
constexpr float kTexelMetres = 0.3f;       // the footprint the hour's sun is read over on a texel (fx::SunVisibility's)
constexpr bgfx::ViewId kPlaceView = render::kPreGameViewBegin + 4;   // the place's sun map
constexpr bgfx::ViewId kComposeView = render::kPreGameViewBegin + 5; // one lightmap a frame
constexpr size_t kFitSamples = 1500;
// A place's bake on disk, in two entries (the app's cache: an entry that is gone is baked again):
//   * its geometry's: per lightmap the sky's visibility on its texels, keyed by the geometry alone (the heavy part,
//     once per town);
//   * its lightmap set's (the game swaps sets by the hour: morning, day1..4, evening): the direction the set was
//     baked with, the sun's light on its texels from there and its tone, keyed by the geometry and the set.
constexpr texture::DerivedShape kSkyShape{.magic = {'S', 'T', 'X', 'C', 'T', 'L', 'S', 'K'}, .version = 1};
constexpr texture::DerivedShape kSetShape{.magic = {'S', 'T', 'X', 'C', 'T', 'L', 'S', 'T'}, .version = 2};
constexpr size_t kMaxPlaceBytes = 64u << 20;

// One cache entry read back in order, or written in order.
struct Entry
{
    std::optional<texture::Blob> blob;
    size_t cursor = 0;
    std::vector<uint8_t> payload;
    bool take(void *out, size_t size)
    {
        if (!blob || cursor + size > blob->size)
            return false;
        std::memcpy(out, blob->data.get() + cursor, size);
        cursor += size;
        return true;
    }
    void put(const void *in, size_t size)
    {
        payload.insert(payload.end(), static_cast<const uint8_t *>(in), static_cast<const uint8_t *>(in) + size);
    }
};

double Since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

uint64_t Hash(uint64_t h, const void *data, size_t size)
{
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
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
    place_sun_ = bounce_sun_ = {0.0f, 0.0f, 0.0f};
    place_job_.reset();
    sun_job_.reset();
    ready_ = false;
    place_prepared_ = false;
    sun_bounce_.reset();
    wall_job_.reset();
    wall_volume_.reset();
    wall_ = {};
    wall_ready_ = false;
    name_ = std::string(name);
    positions_.assign(positions.begin(), positions.end());
    uvs_.assign(uvs.begin(), uvs.end());
    indices_.assign(indices.begin(), indices.end());
    triangle_lightmap_.assign(triangle_lightmap.begin(), triangle_lightmap.end());
    lightmaps_.assign(lightmaps.begin(), lightmaps.end());
    if (indices_.empty())
    {
        if (!name_.empty())
            shim::logf("effects: town light of %s: no lightmapped geometry reported: none here", name_.c_str());
        return;
    }
    if (!enabled_)
        shim::logf("effects: town light of %s: not baked while fx_town_light is off (the preset or the option): baked "
                   "when it is switched on", name_.c_str());
    else if (occluders_name_ == name_ && !occluder_indices_.empty())
        start_place();
    else
        shim::logf("effects: town light of %s: waiting for the place's still geometry (%s reported)", name_.c_str(),
                   occluders_name_.empty() ? "none" : occluders_name_.c_str());
}

void TownLight::set_occluders(std::string_view name, std::span<const std::array<float, 3>> positions,
                              std::span<const std::array<float, 2>> uvs, std::span<const uint32_t> indices,
                              std::vector<fx::Occluders::Cut> cut, std::span<const uint32_t> colors)
{
    occluders_name_ = std::string(name);
    occluder_colors_.assign(colors.begin(), colors.end());
    occluder_positions_.assign(positions.begin(), positions.end());
    occluder_indices_.assign(indices.begin(), indices.end());
    occluder_uvs_.assign(uvs.begin(), uvs.end());
    occluder_cut_ = std::move(cut);
    if (enabled_ && !indices_.empty() && occluders_name_ == name_ && !occluder_indices_.empty() && !place_job_ && maps_.empty())
        start_place();
}

void TownLight::set_enabled(bool enabled)
{
    if (enabled == enabled_)
        return;
    enabled_ = enabled;
    if (!enabled_)
    {
        // Nothing shows our lightmaps now: the bakes still running are cancelled (what is done stays for the next
        // switch on); the walls are calibrated again then.
        place_job_.reset();
        sun_job_.reset();
        if (wall_job_)
        {
            wall_job_.reset();
            wall_volume_.reset();
        }
        return;
    }
    if (!indices_.empty() && occluders_name_ == name_ && !occluder_indices_.empty() && !place_job_ && maps_.empty())
        start_place();
}

void TownLight::start_place()
{
    place_prepared_ = false;
    auto job = std::make_shared<PlaceJob>();
    // The game's lightmaps' textures, looked up here (main thread): the draws are matched by them.
    std::vector<const shim::TexStore *> stores;
    for (const render::EffectsLightmap &m : lightmaps_)
        stores.push_back(shim::store_of(m.texture));
    texture::Service &service = texture::Service::instance();
    place_job_.start("town light of " + name_, job,
                 [job, stores, positions = positions_, uvs = uvs_, indices = indices_, triangle_lightmap = triangle_lightmap_,
                  lightmaps = lightmaps_, occ_positions = occluder_positions_, occ_indices = occluder_indices_,
                  occ_uvs = occluder_uvs_, occ_cut = occluder_cut_, name = name_,
                  directory = service.cache_directory()] {
        const auto started = std::chrono::steady_clock::now();
        uint64_t key = 1469598103934665603ull;
        key = Hash(key, positions.data(), positions.size() * sizeof(positions[0]));
        key = Hash(key, uvs.data(), uvs.size() * sizeof(uvs[0]));
        key = Hash(key, indices.data(), indices.size() * sizeof(uint32_t));
        key = Hash(key, triangle_lightmap.data(), triangle_lightmap.size() * sizeof(uint32_t));
        key = Hash(key, occ_positions.data(), occ_positions.size() * sizeof(occ_positions[0]));
        key = Hash(key, occ_indices.data(), occ_indices.size() * sizeof(uint32_t));
        for (const fx::Occluders::Cut &c : occ_cut)
        {
            const uint8_t solid = c.mask ? 1 : 0;
            key = Hash(key, &solid, 1);
        }
        for (const render::EffectsLightmap &lm : lightmaps)
        {
            const uint32_t size[2] = {lm.width, lm.height};
            key = Hash(key, size, sizeof(size));
        }
        uint64_t set_key = key;
        for (const render::EffectsLightmap &lm : lightmaps)
            set_key = Hash(set_key, lm.data.data(), lm.data.size());
        Entry sky_entry, set_entry;
        if (!directory.empty())
        {
            sky_entry.blob = texture::read_derived_file(directory, key, kSkyShape, kMaxPlaceBytes);
            set_entry.blob = texture::read_derived_file(directory, set_key, kSetShape, kMaxPlaceBytes);
        }
        bool sky_cached = sky_entry.blob.has_value(), set_cached = set_entry.blob.has_value();
        double sky_ms = 0.0, fit_ms = 0.0, sun_ms = 0.0;
        // Only what the rays from the lightmapped ground can reach: a backdrop island kilometres away is left out.
        job->occluders = std::make_shared<const fx::Occluders>(occ_positions, occ_indices, occ_uvs, occ_cut,
                                                               fx::BoundsOf(positions, fx::kSunRayReach));
        const fx::Occluders &occluders = *job->occluders;
        LogExcluded("town light", name, occluders);
        const unsigned threads = Threads();
        std::vector<fx::SunFitSample> samples;
        for (size_t m = 0; m < lightmaps.size(); ++m)
        {
            const render::EffectsLightmap &lm = lightmaps[m];
            uint32_t code; // the .tx format tag is the D3DFORMAT FourCC
            std::memcpy(&code, lm.format.data(), sizeof(code));
            const char kind = lm.format[3];
            Map map;
            map.store = stores[m];
            map.game.resize(static_cast<size_t>(lm.width) * lm.height * 4);
            if (!stores[m] || lm.format[0] != 'D' || !(kind == '1' || kind == '3' || kind == '5') || lm.width < 4 ||
                lm.height < 4 || lm.data.size() < texture::d3d_level_bytes(code, lm.width, lm.height) ||
                !texture::decode_rgba8(code, lm.data.data(), lm.width, lm.height, map.game.data()))
            {
                job->how += " (a lightmap that is no DXT1/3/5 of its size, or has no texture, keeps the game's)";
                continue;
            }
            map.texels = std::make_shared<const fx::LightmapTexels>(fx::RasterizeLightmap(
                positions, uvs, indices, triangle_lightmap, static_cast<uint32_t>(m), static_cast<int>(lm.width),
                static_cast<int>(lm.height)));
            if (map.texels->texel.empty())
                continue;
            map.sky.resize(map.texels->texel.size());
            map.baked.resize(map.texels->texel.size());
            if (!(sky_cached && sky_entry.take(map.sky.data(), map.sky.size())))
            {
                sky_cached = false;
                const auto t = std::chrono::steady_clock::now();
                map.sky = fx::SkyVisibility(occluders, *map.texels, 10.0f, 32, threads);
                sky_ms += Since(t);
            }
            if (!(set_cached && set_entry.take(map.baked.data(), map.baked.size()) && set_entry.take(&map.tone, sizeof(map.tone))))
                set_cached = false;
            // The ground's texels open to the sky, with the game's luminance over its neighbourhood's (the paint's
            // grass and sand are broad, the shadows' edges sharp): what the bake's sun is fitted to.
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
            job->maps.push_back(std::move(map));
        }
        if (samples.size() > kFitSamples)
        {
            std::vector<fx::SunFitSample> spread;
            for (size_t i = 0; i < kFitSamples; ++i)
                spread.push_back(samples[i * samples.size() / kFitSamples]);
            samples.swap(spread);
        }
        auto store = [&](uint64_t entry_key, const texture::DerivedShape &shape, const Entry &entry) {
            if (directory.empty())
                return std::string(" (no cache directory: baked again next time)");
            if (texture::write_derived_file(directory, entry_key, shape, entry.payload.data(), entry.payload.size()) == 0)
                return std::string(" (ERROR: cache write failed: baked again next time)");
            return std::string();
        };
        if (!sky_cached)
        {
            for (const Map &map : job->maps)
                sky_entry.put(map.sky.data(), map.sky.size());
            job->how += store(key, kSkyShape, sky_entry);
        }
        if (!(set_cached && set_entry.take(&job->bake, sizeof(job->bake))))
        {
            set_cached = false;
            auto t = std::chrono::steady_clock::now();
            job->bake = fx::FitBakedSun(occluders, samples, 200.0f, threads);
            fit_ms = Since(t);
            t = std::chrono::steady_clock::now();
            for (Map &map : job->maps)
            {
                map.baked = fx::SunVisibility(occluders, *map.texels, job->bake.dir, 0.3f, 4, threads);
                map.tone = fx::CalibrateTownLight(*map.texels, map.game, map.baked, map.sky);
                set_entry.put(map.baked.data(), map.baked.size());
                set_entry.put(&map.tone, sizeof(map.tone));
            }
            sun_ms = Since(t);
            set_entry.put(&job->bake, sizeof(job->bake));
            job->how += store(set_key, kSetShape, set_entry);
        }
        char how[300];
        std::snprintf(how, sizeof(how),
                      "ready in %.0f ms on %u threads (all but 2 of %u cores): sky %s, lightmap set %s (fit %.0f ms, its sun %.0f ms)",
                      Since(started), threads, std::thread::hardware_concurrency(), sky_cached ? "from the cache" : (std::string("not in the cache: baked in ") +
                                                                                 std::to_string(std::lround(sky_ms)) + " ms").c_str(),
                      set_cached ? "from the cache" : "not in the cache: fitted", fit_ms, sun_ms);
        job->how = how + job->how;
        job->state = 1;
    });
}

void TownLight::start_bounce(const std::array<float, 3> &sun)
{
    auto job = std::make_shared<SunJob>();
    job->sun = sun;
    std::vector<std::shared_ptr<const fx::LightmapTexels>> texels;
    for (const Map &m : maps_)
        texels.push_back(m.texels);
    job->baked_bounce = !maps_.empty() && maps_.front().bounce_baked.empty();
    ++sun_counters_.bounce_bakes;
    sun_job_.start("town sun bounce of " + name_, job, [job, texels, scene = bounce_scene_, bake = bake_dir_] {
        // The sun's bounce for the direction (a white sun: our lightmaps' sun term is relative to it), read one cell
        // off the texels as the shader reads the volume; once per place the bake's sun's too.
        const auto t = std::chrono::steady_clock::now();
        const fx::Vec3 dir{job->sun[0], job->sun[1], job->sun[2]};
        const fx::BounceScene &s = scene->scene;
        auto gather = [&](const fx::Vec3 &towards) {
            return std::make_shared<const fx::BounceField>(fx::GatherBounce(
                s, fx::SunOnSurfels(*scene->occluders, s, towards, {1.0f, 1.0f, 1.0f}, Threads()), Threads()));
        };
        job->field = gather(dir);
        for (size_t m = 0; m < texels.size(); ++m)
            job->bounce.push_back(fx::TexelBounce(*texels[m], *job->field, s.grid.cell, Threads()));
        if (job->baked_bounce)
        {
            const std::shared_ptr<const fx::BounceField> baked = gather(bake);
            for (size_t m = 0; m < texels.size(); ++m)
                job->bounce_baked.push_back(fx::TexelBounce(*texels[m], *baked, s.grid.cell, Threads()));
        }
        job->bounce_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        job->state = 1;
    });
}

void TownLight::set_volume(const fx::VisibilityVolume *volume)
{
    if (!enabled_ || !volume || volume->Empty() || wall_volume_ || maps_.empty() || !occluders_ ||
        occluder_colors_.size() != occluder_positions_.size())
        return;
    wall_volume_ = std::make_shared<const fx::VisibilityVolume>(*volume);
    // The walls are the still geometry's triangles that are not under a lightmap (those are relit as texels):
    // matched by their centroids, to the centimetre.
    auto job = std::make_shared<WallJob>();
    wall_light_ = maps_.front().tone;
    wall_job_.start("town walls of " + name_, job,
                                      [job, volume = wall_volume_, occluders = occluders_, positions = occluder_positions_,
                                       indices = occluder_indices_, colors = occluder_colors_, lm_positions = positions_,
                                       lm_indices = indices_, tone = maps_.front().tone, bake = bake_dir_] {
        const auto started = std::chrono::steady_clock::now();
        auto key = [](const fx::Vec3 &a, const fx::Vec3 &b, const fx::Vec3 &c) {
            int64_t q[3];
            for (int k = 0; k < 3; ++k)
                q[k] = std::llround((a[k] + b[k] + c[k]) * (100.0f / 3.0f));
            return (static_cast<uint64_t>(q[0]) * 73856093u) ^ (static_cast<uint64_t>(q[1]) * 19349663u) ^
                   (static_cast<uint64_t>(q[2]) * 83492791u);
        };
        std::unordered_set<uint64_t> lit;
        for (size_t t = 0; t + 2 < lm_indices.size(); t += 3)
            lit.insert(key(lm_positions[lm_indices[t]], lm_positions[lm_indices[t + 1]], lm_positions[lm_indices[t + 2]]));
        std::vector<uint8_t> skip(indices.size() / 3, 0);
        for (size_t t = 0; t < skip.size(); ++t)
            skip[t] = lit.count(key(positions[indices[3 * t]], positions[indices[3 * t + 1]], positions[indices[3 * t + 2]])) ? 1 : 0;
        job->wall = fx::CalibrateTownWalls(positions, colors, indices, skip, *volume, tone, bake, *occluders, Threads());
        job->ms = Since(started);
        job->state = 1;
    });
}

void TownLight::update(const std::array<float, 3> &sun, const std::array<float, 3> &sun_color, bool present)
{
    sun_present_ = present;
    live_sun_ = sun;
    live_sun_luminance_ = std::max(0.299f * sun_color[0] + 0.587f * sun_color[1] + 0.114f * sun_color[2], 1e-3f);
    if (!enabled_)
        return; // no bake runs (set_enabled)
    if (wall_job_ && wall_job_->state.load() == 1)
    {
        wall_ = wall_job_->wall;
        wall_ready_ = true;
        const fx::TownWallTone &w = wall_;
        shim::logf("effects: town light of %s: walls %s in %.0f ms: %u vertices, %u in the bake's shade; their light per "
                   "bin of our sky over the open ones: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f (counts %u %u %u %u %u %u %u "
                   "%u), the game's sky %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
                   name_.c_str(), w.valid ? "calibrated" : "NOT calibrated (too few shaded open vertices): not relit",
                   wall_job_->ms, w.vertices, w.shaded, w.relative[0], w.relative[1], w.relative[2], w.relative[3],
                   w.relative[4], w.relative[5], w.relative[6], w.relative[7], w.count[0], w.count[1], w.count[2],
                   w.count[3], w.count[4], w.count[5], w.count[6], w.count[7], w.game_sky[0], w.game_sky[1],
                   w.game_sky[2], w.game_sky[3], w.game_sky[4], w.game_sky[5], w.game_sky[6], w.game_sky[7]);
        wall_job_.reset();
    }
    if (place_job_ && place_job_->state.load() == 1)
    {
        maps_ = std::move(place_job_->maps);
        occluders_ = std::move(place_job_->occluders);
        size_t texels = 0, valid = 0;
        for (const Map &m : maps_)
        {
            texels += m.texels->texel.size();
            valid += m.tone.valid ? 1 : 0;
        }
        const fx::SunFit &bake = place_job_->bake;
        bake_dir_ = bake.dir;
        shim::logf("effects: town light of %s: %zu lightmaps (%zu texels, %zu relit), the game's baked with the sun at "
                   "(%.2f, %.2f, %.2f), %.0f° up (r %.2f), %s",
                   name_.c_str(), maps_.size(), texels, valid, bake.dir[0], bake.dir[1], bake.dir[2],
                   std::asin(std::clamp(bake.dir[1], -1.0f, 1.0f)) * 57.29578f, bake.r, place_job_->how.c_str());
        for (const Map &m : maps_)
            shim::logf("effects: town light of %s: a lightmap of %dx%d lit %.3f + %.3f sun + %.3f sky (R² %.2f, sun from %u "
                       "shadow edges), the game's sky for ours 0.06..0.94: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f%s",
                       name_.c_str(), m.texels->width, m.texels->height, m.tone.a, m.tone.b, m.tone.c, m.tone.r2, m.tone.edges,
                       m.tone.game_sky[0], m.tone.game_sky[1], m.tone.game_sky[2], m.tone.game_sky[3], m.tone.game_sky[4],
                       m.tone.game_sky[5], m.tone.game_sky[6], m.tone.game_sky[7],
                       m.tone.valid ? "" : ": not explained, keeps the game's");
        // A lightmap whose tone the game's does not explain keeps the game's.
        std::erase_if(maps_, [](const Map &m) { return !m.tone.valid; });
        place_prepared_ = true;
        place_job_.reset();
    }
    if (maps_.empty())
        return;
    // Prepare the current night/indoor state too. Waiting for a future sunrise here
    // leaves a legitimately sunless location permanently behind the readiness fader.
    // The hour's sun itself is render()'s. Here its bounce: a job per kBounceStepDegrees while the place has a scene.
    const std::array<float, 3> target = present ? sun : std::array<float, 3>{0.0f, -1.0f, 0.0f};
    if (sun_job_ && sun_job_->state.load() == 1)
    {
        for (size_t m = 0; m < maps_.size() && m < sun_job_->bounce.size(); ++m)
        {
            Map &map = maps_[m];
            if (m < sun_job_->bounce_baked.size())
            {
                map.bounce_baked = std::move(sun_job_->bounce_baked[m]);
                if (map.relight != fx::SunRelight::kNoMap)
                    upload_map(map); // the bake's sun's bounce is one of its static inputs
            }
            map.bounce = std::move(sun_job_->bounce[m]);
            upload_bounce(map);
            map.compose_due = true;
        }
        sun_bounce_ = sun_job_->field;
        bounce_sun_ = sun_job_->sun;
        ++generation_;
        shim::logf("effects: town light of %s: sun bounce %u for (%.3f, %.3f, %.3f), %.1f° up, in %.0f ms on %u threads%s",
                   name_.c_str(), generation_, bounce_sun_[0], bounce_sun_[1], bounce_sun_[2],
                   std::asin(std::clamp(bounce_sun_[1], -1.0f, 1.0f)) * 57.29578f, sun_job_->bounce_ms, Threads(),
                   sun_job_->baked_bounce ? " (with the bake's sun's)" : "");
        sun_job_.reset();
    }
    if (!sun_job_ && bounce_scene_ && Angle(target, bounce_sun_) > kBounceStepDegrees)
        start_bounce(target);
}

void TownLight::upload_map(Map &map)
{
    // The texels' static inputs as full grids (the gutter takes its covered texel's): what the compose pass reads.
    const fx::LightmapTexels &texels = *map.texels;
    const size_t count = static_cast<size_t>(texels.width) * static_cast<size_t>(texels.height);
    std::vector<float> position(count * 4, 0.0f);
    std::vector<uint8_t> normal_sky(count * 4, 0), baked(count * 4, 0);
    const bool bounced = map.bounce_baked.size() == texels.texel.size() * 3;
    for (size_t i = 0; i < texels.texel.size(); ++i)
    {
        const size_t at = static_cast<size_t>(texels.texel[i]) * 4;
        for (int k = 0; k < 3; ++k)
        {
            position[at + k] = texels.position[i][k];
            normal_sky[at + k] = static_cast<uint8_t>(std::lround(std::clamp(texels.normal[i][k] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
            baked[at + k] = bounced ? map.bounce_baked[i * 3 + k] : 0;
        }
        position[at + 3] = 1.0f;
        normal_sky[at + 3] = map.sky[i];
        baked[at + 3] = map.baked[i];
    }
    for (size_t g = 0; g + 1 < texels.gutter.size(); g += 2)
    {
        const size_t to = static_cast<size_t>(texels.gutter[g]) * 4, from = static_cast<size_t>(texels.gutter[g + 1]) * 4;
        std::copy_n(&position[from], 4, &position[to]);
        std::copy_n(&normal_sky[from], 4, &normal_sky[to]);
        std::copy_n(&baked[from], 4, &baked[to]);
    }
    if (map.relight == fx::SunRelight::kNoMap)
        map.relight = relight_.AddMap({texels.width, texels.height, position, normal_sky, baked, map.game});
    else
        relight_.SetBakedBounce(map.relight, baked);
    map.compose_due = true;
}

void TownLight::upload_bounce(Map &map)
{
    const fx::LightmapTexels &texels = *map.texels;
    if (map.relight == fx::SunRelight::kNoMap)
        return;
    if (map.bounce.size() != texels.texel.size() * 3 || map.bounce_baked.size() != map.bounce.size())
    {
        relight_.SetBounce(map.relight, {});
        return;
    }
    std::vector<uint8_t> grid(static_cast<size_t>(texels.width) * static_cast<size_t>(texels.height) * 4, 0);
    for (size_t i = 0; i < texels.texel.size(); ++i)
        std::copy_n(&map.bounce[i * 3], 3, &grid[static_cast<size_t>(texels.texel[i]) * 4]);
    for (size_t g = 0; g + 1 < texels.gutter.size(); g += 2)
        std::copy_n(&grid[static_cast<size_t>(texels.gutter[g + 1]) * 4], 4, &grid[static_cast<size_t>(texels.gutter[g]) * 4]);
    relight_.SetBounce(map.relight, grid);
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
            upload_bounce(map);
            if (map.relight == fx::SunRelight::kNoMap)
            {
                shim::logf("ERROR: effects: town light of %s: a lightmap of %dx%d could not be set up for the relight: "
                           "the game's stays", name_.c_str(), map.texels->width, map.texels->height);
                map.compose_due = false;
            }
        }
    // The place's sun map: when the sun moved a step, or what it was drawn with changed.
    const bool up = sun_present_ && live_sun_[1] > 0.0f;
    const uint64_t cut = shadows.cut_signature();
    const bool place_void = place_sun_ == std::array<float, 3>{0.0f, 0.0f, 0.0f} || !relight_.PlaceDrawn() || cut != place_cut_signature_;
    if (up && (place_void || Angle(live_sun_, place_sun_) > fx::kSunStepDegrees))
    {
        const fx::SunPlace place = fx::ComputeSunPlace({live_sun_[0], live_sun_[1], live_sun_[2]}, positions_, fx::kSunRayReach,
                                                       fx::SunRelight::kPlaceSize);
        relight_.BeginPlace(kPlaceView, place, {0.0f, 0.0f, 0.0f});
        shadows.draw_tiles(device, casters, kPlaceView, {0.0f, 0.0f, 0.0f},
                           [&place](const fx::Vec3 &center, float radius) { return place.Reaches(center, radius); });
        place_sun_ = live_sun_;
        place_cut_signature_ = shadows.cut_signature();
        ++sun_counters_.place_draws;
        compose_all();
    }
    if (up == night_)
    {
        night_ = !up;
        compose_all();
    }
    // One lightmap a frame (one view): a town has one to four.
    for (Map &map : maps_)
        if (map.compose_due && map.relight != fx::SunRelight::kNoMap)
        {
            fx::RelightTone tone{map.tone.a, map.tone.b, map.tone.c, {}};
            std::copy_n(map.tone.game_sky, fx::TownLightTone::kSkyBins, tone.game_sky);
            relight_.Compose(kComposeView, map.relight, tone, up, 0.35f * kTexelMetres, debug_);
            map.compose_due = false;
            ++sun_counters_.composes;
            break;
        }
    // Ours replace the game's once every lightmap has been composed (the textures exist from the next frame on).
    if (!ready_ && std::none_of(maps_.begin(), maps_.end(), [](const Map &m) { return m.compose_due; }))
    {
        ready_ = true;
        shim::logf("effects: town light of %s: %zu lightmaps relit for the sun at (%.3f, %.3f, %.3f), %.1f° up", name_.c_str(),
                   maps_.size(), live_sun_[0], live_sun_[1], live_sun_[2],
                   std::asin(std::clamp(live_sun_[1], -1.0f, 1.0f)) * 57.29578f);
    }
}

void TownLight::set_bounce_scene(std::shared_ptr<const Bounce::Scene> scene)
{
    if (scene == bounce_scene_)
        return;
    bounce_scene_ = std::move(scene);
    sun_bounce_.reset();
    sun_job_.reset(); // of the scene that went
    for (Map &map : maps_)
    {
        const bool had = !map.bounce_baked.empty();
        map.bounce_baked.clear();
        map.bounce.clear();
        if (had && map.relight != fx::SunRelight::kNoMap)
        {
            upload_map(map);
            upload_bounce(map);
        }
    }
    bounce_sun_ = {0.0f, 0.0f, 0.0f}; // the next update() bakes the new scene's
}

void TownLight::set_debug(bool debug)
{
    if (debug == debug_)
        return;
    debug_ = debug;
    compose_all();
}

bgfx::TextureHandle TownLight::replacement(const shim::TexStore *store)
{
    ++counts_[0];
    if (!active() || !store)
        return BGFX_INVALID_HANDLE;
    for (const Map &map : maps_)
        if (map.store == store)
        {
            const bgfx::TextureHandle ours = relight_.Texture(map.relight);
            counts_[1] += bgfx::isValid(ours) ? 1 : 0;
            return ours;
        }
    return BGFX_INVALID_HANDLE;
}

bool TownLight::relit(const shim::TexStore *store) const
{
    if (!active() || !store)
        return false;
    for (const Map &map : maps_)
        if (map.store == store)
            return bgfx::isValid(relight_.Texture(map.relight));
    return false;
}

float TownLight::sun_shade(const shim::TexStore *store, float unmeasured) const
{
    if (!active() || !store)
        return unmeasured;
    for (const Map &map : maps_)
        if (map.store == store && bgfx::isValid(relight_.Texture(map.relight)))
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
    wall_job_.reset();
    wall_volume_.reset();
    wall_ready_ = false;
    place_job_.reset();
    sun_job_.reset();
    ready_ = false;
    place_prepared_ = false;
}

void TownLight::on_device_lost()
{
    relight_.OnDeviceLost();
    for (Map &map : maps_)
    {
        map.relight = fx::SunRelight::kNoMap; // went with the device: render() uploads and composes them again
        map.compose_due = true;
    }
    place_sun_ = {0.0f, 0.0f, 0.0f};
    ready_ = false; // the game's lightmaps until ours are composed again
}

} // namespace storm::effects
