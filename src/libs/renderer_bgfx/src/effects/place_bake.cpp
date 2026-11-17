#include "effects/place_bake.hpp"

#include "effects/casters.hpp"
#include "shim/shim.hpp"
#include "texture/core/cache.hpp"
#include "texture/core/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace storm::effects
{

namespace
{
constexpr texture::DerivedShape kShape{.magic = {'S', 'T', 'X', 'C', 'P', 'L', 'A', 'C'}, .version = fx::kPlaceBakeVersion};
constexpr size_t kMaxFileBytes = 96u << 20;
// Outdoors: cells of half a metre (doorways, galleries, a deck's 3 m), grown under the budget (towns: about 1.3 m).
constexpr float kOutdoorCell = 0.5f;
constexpr uint32_t kOutdoorCells = 1u << 19;
// A room: the occlusion's cells (under a table, behind a barrel).
constexpr float kRoomCell = 0.2f;
constexpr uint32_t kRoomCells = 1u << 20;
constexpr float kAoRadius = 1.5f;      // metres: occluders farther than this do not count
constexpr float kNearSky = 10.0f;      // metres: the sky of the game's lightmaps
constexpr float kFocusMargin = 8.0f;   // metres around a town's ground (its walls stand on its edge)
constexpr float kFocusAir = 25.0f;     // metres above its highest ground: roofs, towers, palms
constexpr float kMinOffset = 0.5f;     // metres: the receivers read the visibility at least this far off their surface

double Since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
} // namespace

std::shared_ptr<const fx::Occluders> PlaceRays::get(double *build_ms)
{
    std::lock_guard lock(mutex_);
    if (build_ms)
        *build_ms = 0.0;
    if (!occluders_)
    {
        const auto started = std::chrono::steady_clock::now();
        occluders_ = std::make_shared<const fx::Occluders>(place_->positions, place_->indices, place_->uvs, place_->cut);
        if (build_ms)
            *build_ms = Since(started);
    }
    return occluders_;
}

void PlaceRays::release()
{
    std::lock_guard lock(mutex_);
    occluders_.reset();
}

std::string PlaceStore::read()
{
    if (directory_.empty())
        return {};
    const std::optional<texture::Blob> blob = texture::read_derived_file(directory_, key_, kShape, kMaxFileBytes);
    if (!blob)
        return {};
    std::string error;
    std::optional<fx::PlaceFile> file = fx::PlaceFile::Decode({blob->data.get(), blob->size}, key_, error);
    if (!file)
        return error;
    std::lock_guard lock(mutex_);
    file_ = std::move(*file);
    return {};
}

std::vector<uint8_t> PlaceStore::find(fx::PlaceSection kind, uint64_t key)
{
    std::lock_guard lock(mutex_);
    const std::span<const uint8_t> bytes = file_.Find(kind, key);
    return {bytes.begin(), bytes.end()};
}

std::string PlaceStore::put(fx::PlaceSection kind, uint64_t key, std::vector<uint8_t> bytes, size_t keep)
{
    std::lock_guard lock(mutex_);
    file_.Put(kind, key, std::move(bytes), keep);
    if (directory_.empty())
        return "no cache directory: baked again next time";
    const std::vector<uint8_t> encoded = file_.Encode(key_);
    if (encoded.size() > kMaxFileBytes)
        return "ERROR: too large for the cache: baked again next time";
    if (texture::write_derived_file(directory_, key_, kShape, encoded.data(), encoded.size()) == 0)
        return "ERROR: cache write failed: baked again next time";
    return {};
}

fx::ProbeParams PlaceBake::params_for(bool interior, bool room, const std::array<float, 3> &room_lo,
                                      const std::array<float, 3> &room_hi) const
{
    fx::ProbeParams params;
    params.cell = kOutdoorCell;
    params.max_cells = kOutdoorCells;
    if (interior)
    {
        if (room)
        {
            params.lo = {room_lo[0], room_lo[1], room_lo[2]};
            params.hi = {room_hi[0], room_hi[1], room_hi[2]};
            params.cell = kRoomCell;
            params.max_cells = kRoomCells;
            params.turned = false;
            params.ao_radius = kAoRadius;
            params.walk = walk_;
        }
        return params;
    }
    if (place_ && focus_name_ == place_->name && focus_hi_[0] > focus_lo_[0])
    {
        params.lo = {focus_lo_[0] - kFocusMargin, focus_lo_[1] - 2.0f, focus_lo_[2] - kFocusMargin};
        params.hi = {focus_hi_[0] + kFocusMargin, focus_hi_[1] + kFocusAir, focus_hi_[2] + kFocusMargin};
        params.near_sky = kNearSky;
    }
    return params;
}

void PlaceBake::set_geometry(const render::EffectsLocationGeometry &geometry)
{
    const bool room = geometry.interior && geometry.room_hi[0] > geometry.room_lo[0] && geometry.room_hi[1] > geometry.room_lo[1] &&
                      geometry.room_hi[2] > geometry.room_lo[2];
    if (geometry.indices.empty())
    {
        if (place_)
        {
            place_.reset();
            rebuild();
        }
        return;
    }
    const auto same = [](const auto &kept, const auto &given) {
        return kept.size() == given.size() && std::equal(kept.begin(), kept.end(), given.begin());
    };
    if (place_ && place_->name == geometry.name && place_->interior == geometry.interior && room == room_ &&
        geometry.room_lo == room_lo_ && geometry.room_hi == room_hi_ && same(place_->positions, geometry.positions) &&
        same(place_->indices, geometry.indices) && same(walk_, geometry.walk))
        return; // the same location again: its bakes stay
    auto place = std::make_shared<Place>();
    place->name = std::string(geometry.name);
    place->interior = geometry.interior;
    place->positions.assign(geometry.positions.begin(), geometry.positions.end());
    place->uvs.assign(geometry.uvs.begin(), geometry.uvs.end());
    place->indices.assign(geometry.indices.begin(), geometry.indices.end());
    place->colors.assign(geometry.colors.begin(), geometry.colors.end());
    // Where a texture's alpha cuts holes (foliage, nets, gratings) the rays pass as the colour pass shows through
    // (effects/casters.hpp: one rule for every pass).
    if (place->uvs.size() == place->positions.size())
        for (const auto &surface : geometry.surfaces)
        {
            const shim::TexStore *store = shim::store_of(surface.texture);
            const CasterMaterial material = location_caster_material(store);
            if (material.cut <= 0.0f || surface.first_index + surface.index_count > place->indices.size())
                continue;
            if (place->cut.empty())
                place->cut.resize(place->indices.size() / 3);
            for (uint32_t t = surface.first_index / 3; t < (surface.first_index + surface.index_count) / 3; ++t)
                place->cut[t] = {store->cutout, material.cut};
        }
    room_ = room;
    room_lo_ = geometry.room_lo;
    room_hi_ = geometry.room_hi;
    walk_.assign(geometry.walk.begin(), geometry.walk.end());
    place_ = std::move(place);
    rebuild();
}

void PlaceBake::set_focus(std::string_view name, const std::array<float, 3> &lo, const std::array<float, 3> &hi)
{
    if (focus_name_ == name && focus_lo_ == lo && focus_hi_ == hi)
        return;
    focus_name_ = std::string(name);
    focus_lo_ = lo;
    focus_hi_ = hi;
    if (place_ && !place_->interior)
        rebuild(); // the cells move to the ground (before the first load when both come in one frame)
}

void PlaceBake::rebuild()
{
    job_.reset(); // a worker still running finishes into its own job, which nobody reads
    rays_.reset();
    ready_.reset();
    visibility_.reset();
    ao_.reset();
    state_ = place_ && wanted_ ? State::Building : State::None;
}

void PlaceBake::start()
{
    rays_ = std::make_shared<PlaceRays>(place_);
    auto job = std::make_shared<Job>();
    job_.start("place " + place_->name, job,
               [job, place = std::shared_ptr<const Place>(place_), rays = rays_,
                probes = params_for(place_->interior, room_, room_lo_, room_hi_), directory = texture::Service::instance().cache_directory()](fx::BakeStage stage) {
        const auto started = std::chrono::steady_clock::now();
        if (stage == fx::BakeStage::Load)
        {
            // The one hash of the place: everything its rays depend on.
            const uint64_t key = fx::PlaceKey({place->positions, place->indices, place->uvs, place->cut}, probes);
            job->store = std::make_shared<PlaceStore>(directory, key);
            const double hash_ms = Since(started);
            const std::string refused = job->store->read();
            if (!refused.empty())
                shim::logf("ERROR: effects: place %s: its cache file is refused (%s): baked again", place->name.c_str(),
                           refused.c_str());
            const std::vector<uint8_t> bytes = job->store->find(fx::PlaceSection::Probes, key);
            if (bytes.empty())
                return false; // not in the cache: baked on the bake thread
            std::string error;
            std::optional<fx::PlaceProbes> probes = fx::DecodeProbes(bytes, error);
            if (!probes)
            {
                shim::logf("ERROR: effects: place %s: its cached probes are refused (%s): baked again", place->name.c_str(),
                           error.c_str());
                return false;
            }
            job->probes = std::move(*probes);
            char how[120];
            std::snprintf(how, sizeof(how), "from the cache in %.0f ms (key hashed in %.1f ms)", Since(started), hash_ms);
            job->how = how;
            job->state = 1;
            return true;
        }
        double build_ms = 0.0;
        const std::shared_ptr<const fx::Occluders> occluders = rays->get(&build_ms);
        fx::ProbeParams params = probes;
        params.threads = fx::BakeThreads();
        const auto traced = std::chrono::steady_clock::now();
        job->probes = fx::BakePlaceProbes(*occluders, place->positions, place->indices, params);
        if (job->probes.Empty())
        {
            job->state = 2;
            return true;
        }
        const double trace_ms = Since(traced);
        char how[280];
        std::snprintf(how, sizeof(how),
                      "not in the cache: baked in %.0f ms (ray structure %.0f ms on one thread, %.1f MB; %.1f million rays in "
                      "%.0f ms on up to %u threads: %.1f core-seconds, %.2f us a ray)",
                      Since(started), build_ms, static_cast<double>(occluders->Bytes()) / 1048576.0,
                      static_cast<double>(job->probes.rays) * 1e-6, trace_ms, params.threads, job->probes.cpu_ms / 1000.0,
                      job->probes.cpu_ms * 1000.0 / static_cast<double>(std::max<uint64_t>(1, job->probes.rays)));
        job->how = how;
        if (!job->probes.ao.Empty())
        {
            char lift[80];
            std::snprintf(lift, sizeof(lift), "; occlusion raised %.2f times for the walked floor", job->probes.walk_lift);
            job->how += lift;
        }
        const std::string unsaved = job->store->put(fx::PlaceSection::Probes, job->store->key(), fx::EncodeProbes(job->probes));
        if (!unsaved.empty())
            job->how += " (" + unsaved + ")";
        job->state = 1;
        return true;
    });
}

void PlaceBake::take(Job &job)
{
    const std::string &name = place_->name;
    const fx::VisibilityVolume &v = job.probes.visibility;
    if (!visibility_.set(v, std::max(v.cell, kMinOffset)))
    {
        shim::logf("ERROR: effects: place %s: the %dx%dx%d RG8 3D texture of its visibility cannot be created: everything "
                   "here counts as under open sky",
                   name.c_str(), v.size[0], v.size[1], v.size[2]);
        state_ = State::Failed;
        return;
    }
    const bool lobes = !job.probes.ao.Empty();
    if (lobes && !ao_.set(job.probes.ao))
        shim::logf("ERROR: effects: place %s: the %dx%dx%d RGBA8 3D texture of its occlusion cannot be created: no ambient "
                   "occlusion here",
                   name.c_str(), v.size[0], v.size[1], v.size[2]);
    shim::logf("effects: place %s: probes of %dx%dx%d cells of %.2f m from y %.1f to %.1f over %zu drawn triangles%s%s, %s",
               name.c_str(), v.size[0], v.size[1], v.size[2], v.cell, v.origin[1], v.origin[1] + v.cell * v.size[1],
               place_->indices.size() / 3, lobes ? ", with the occlusion lobes" : "",
               job.probes.near_sky.empty() ? "" : ", with the near sky of the lightmaps", job.how.c_str());
    auto ready = std::make_shared<PlaceReady>();
    ready->place = place_;
    ready->rays = rays_;
    ready->store = job.store;
    if (!place_->interior)
    {
        // The town light reads them on the CPU; an interior keeps only its textures (after a lost device its file is
        // read again).
        ready->volume = std::make_shared<const fx::VisibilityVolume>(std::move(job.probes.visibility));
        ready->near_sky = std::make_shared<const std::vector<uint8_t>>(std::move(job.probes.near_sky));
    }
    ready_ = std::move(ready);
    state_ = State::Ready;
}

void PlaceBake::update(bool wanted, bool keep_rays)
{
    if (wanted != wanted_)
    {
        wanted_ = wanted;
        if (!wanted && place_ && state_ != State::None)
            shim::logf("effects: place %s: its probes are dropped, nothing reads them with these settings (no per-pixel or "
                       "point lighting): made again when something does",
                       place_->name.c_str());
        rebuild();
    }
    if (state_ == State::Ready && rays_ && !keep_rays)
        rays_->release();
    if (state_ != State::Building)
        return;
    if (!job_)
        start();
    switch (job_->state.load())
    {
    case 1:
        take(*job_);
        job_.reset();
        break;
    case 2:
        shim::logf("ERROR: effects: place %s: nothing to lay probes over (%zu positions, %zu indices, or all of it outside "
                   "its box): everything here counts as under open sky",
                   place_->name.c_str(), place_->positions.size(), place_->indices.size());
        state_ = State::Failed;
        job_.reset();
        break;
    default:
        break; // on the load or the bake thread
    }
}

void PlaceBake::shutdown()
{
    job_.reset();
    rays_.reset();
    ready_.reset();
    visibility_.reset();
    ao_.reset();
    state_ = place_ && wanted_ ? State::Building : State::None;
}

} // namespace storm::effects
