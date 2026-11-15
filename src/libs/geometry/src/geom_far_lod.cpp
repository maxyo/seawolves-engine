#include "geom_far_lod.hpp"
#include "geom_far_lod_job.hpp"

#include <storm/render/far_lod.hpp>
#include <storm/render/perf_stats.hpp>
#include <storm/render/texture_cache.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <spdlog/spdlog.h>
#include <strings.h>

namespace far_lod = storm::geometry::far_lod;
namespace render_lod = storm::render::far_lod;

namespace
{
constexpr char kCacheMagic[8] = {'S', 'T', 'M', 'F', 'L', 'O', 'D', '1'};
constexpr uint64_t kKeySeed = 0x464152204c4f4453ull; // "FAR LODS"
constexpr size_t kMaxCacheBytes = 256u << 20;
// A cluster goes to a coarser level only when that level's error is this much under the limit, so a cluster at the
// threshold distance does not switch back and forth.
constexpr float kHysteresis = 1.25f;

storm::render::DerivedDataShape cacheShape(const GeomFarLod::Source &source)
{
    storm::render::DerivedDataShape shape;
    std::memcpy(shape.magic, kCacheMagic, sizeof(shape.magic));
    shape.version = far_lod::kVersion;
    shape.width = static_cast<uint32_t>(source.triangles.size() / 3);
    shape.height = static_cast<uint32_t>(source.objects.size());
    return shape;
}

// What the LODs are made from: the objects, the triangles and of every vertex its position, normal and uv0 (not its
// colour: the baked lighting differs per time of day and does not change the LODs).
uint64_t cacheKey(const GeomFarLod::Source &source)
{
    uint64_t key = storm::render::DerivedDataHash(&far_lod::kVersion, sizeof(far_lod::kVersion), kKeySeed);
    key = storm::render::DerivedDataHash(source.objects.data(), source.objects.size() * sizeof(far_lod::MeshObject), key);
    key = storm::render::DerivedDataHash(source.triangles.data(), source.triangles.size() * sizeof(uint16_t), key);
    std::vector<uint8_t> shape;
    for (size_t b = 0; b < source.buffers.size(); ++b)
    {
        const uint32_t stride = source.strides[b];
        const size_t count = source.buffers[b].size() / stride;
        shape.resize(count * 32);
        for (size_t v = 0; v < count; ++v)
        {
            const uint8_t *vertex = source.buffers[b].data() + v * stride;
            std::memcpy(&shape[32 * v], vertex, 24);           // position, normal
            std::memcpy(&shape[32 * v + 24], vertex + 28, 8);  // uv0
        }
        key = storm::render::DerivedDataHash(shape.data(), shape.size(), key);
    }
    return key;
}

std::vector<far_lod::MeshBuffer> views(const GeomFarLod::Source &source)
{
    std::vector<far_lod::MeshBuffer> result;
    for (size_t b = 0; b < source.buffers.size(); ++b)
        result.push_back({source.buffers[b].data(), source.strides[b],
                          static_cast<uint32_t>(source.buffers[b].size() / source.strides[b])});
    return result;
}

} // namespace

struct GeomFarLod::Job
{
    Source source;
    // 0 queued or being made, 1 ready, 2 failed, 3 dropped before it finished (the bakes shut down with the device:
    // the app went to the background): queued again at the next frame begin.
    std::atomic<int> state{0};
    std::optional<uint64_t> key; // the cache key, computed by the first stage that runs
    std::optional<far_lod::Lods> lods;
    std::string failure;
    bool from_cache = false;
    bool cache_checked = false;
    double ms = 0;
};

namespace
{
// The LODs from the cache, or nothing. A damaged entry is logged (and made again).
bool readCache(GeomFarLod::Job &job)
{
    if (job.cache_checked)
        return false;
    job.cache_checked = true;
    const auto started = std::chrono::steady_clock::now();
    const auto data = storm::render::ReadDerivedData(*job.key, cacheShape(job.source), kMaxCacheBytes);
    if (!data)
        return false;
    job.lods = far_lod::Decode(*data, job.source.objects);
    if (!job.lods)
    {
        spdlog::error("far LOD: the cache entry of {} does not decode into its objects: made again", job.source.name);
        return false;
    }
    job.from_cache = true;
    job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

// Done (ready or failed): the copies of the model's data go, only the object table stays.
void finish(GeomFarLod::Job &job, int state)
{
    // Move-assign empty vectors: initializer-list assignment would keep the triangle allocation alive.
    job.source.buffers = decltype(job.source.buffers){};
    job.source.strides = decltype(job.source.strides){};
    job.source.triangles = decltype(job.source.triangles){};
    job.state = state;
}
} // namespace

bool GeomFarLod::Wanted(const char *file_name, float radius, bool animated)
{
    std::string_view model(file_name);
    constexpr std::string_view kModels = "resource\\models\\";
    if (model.size() > kModels.size() && strncasecmp(file_name, kModels.data(), kModels.size()) == 0)
        model.remove_prefix(kModels.size());
    return far_lod::Wanted(model, radius, animated);
}

GeomFarLod::GeomFarLod(Source source, GEOM_SERVICE &srv) : srv_(srv), name_(source.name)
{
    job_ = std::make_shared<Job>();
    job_->source = std::move(source);
    owner_ = storm::render::FrameOwner::Get();
    if (!owner_)
    {
        spdlog::error("far LOD: {} has no active LOD frame service: it is drawn whole", name_);
        job_.reset();
        owner_ = nullptr;
        return;
    }
    owner_->AddClient(this);
    try
    {
        post(); // Even models never drawn release their source when the job finishes.
    }
    catch (...)
    {
        owner_->RemoveClient(this);
        throw;
    }
}

void GeomFarLod::post()
{
    auto guard = std::make_shared<GeomFarLodJobGuard<Job>>(job_);
    ticket_ = storm::fx::PostBake("far LOD " + name_, [guard](storm::fx::BakeStage stage) {
        Job &job = *guard->job;
        if (stage == storm::fx::BakeStage::Load)
        {
            try
            {
                if (!job.key)
                    job.key = cacheKey(job.source);
                if (!readCache(job))
                    return false;
            }
            catch (const std::exception &error)
            {
                spdlog::error("far LOD: reading the cache entry of {} failed ({}): made again", job.source.name,
                              error.what());
                return false;
            }
            finish(job, 1);
            return true;
        }
        const auto started = std::chrono::steady_clock::now();
        try
        {
            if (!job.key)
                job.key = cacheKey(job.source);
            const auto buffers = views(job.source);
            job.lods = far_lod::Build(job.source.objects, buffers, job.source.triangles,
                                      [](size_t count, const std::function<void(size_t, size_t)> &work) {
                                          storm::fx::ParallelRanges(count, storm::fx::BakeThreads(), work, 1);
                                      });
            job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        catch (const std::exception &error)
        {
            job.failure = error.what();
            finish(job, 2);
            return true;
        }
        // Persistence is optional: failure must not discard a successful build.
        try
        {
            if (!storm::render::DerivedDataCacheEnabled())
                spdlog::error("far LOD: no cache directory: the LODs of {} are made again on every load", job.source.name);
            else
            {
                const std::vector<uint8_t> encoded = far_lod::Encode(*job.lods);
                if (!storm::render::WriteDerivedData(*job.key, cacheShape(job.source), encoded))
                    spdlog::error("far LOD: cannot write the LODs of {} into the cache", job.source.name);
            }
        }
        catch (const std::exception &error)
        {
            spdlog::error("far LOD: caching the LODs of {} failed ({}): keeping the in-memory LODs",
                          job.source.name, error.what());
        }
        finish(job, 1);
        return true;
    }, storm::fx::BakePriority::Background); // the place's own bakes (its probes, its lightmaps) go first
    posted_ = true;
    ++render_lod::pending;
}

GeomFarLod::~GeomFarLod()
{
    if (owner_)
        owner_->RemoveClient(this);
    ticket_.reset(); // cancels the job if it is still queued or running
    if (job_ && posted_)
        --render_lod::pending;
    releaseBuffers();
}

void GeomFarLod::releaseBuffers()
{
    if (static_buffer_ != -1)
        srv_.ReleaseIndexBuffer(static_buffer_);
    static_buffer_ = -1;
    for (auto &passes : slots_)
        for (Slot &slot : passes)
            if (slot.buffer != -1)
                srv_.ReleaseIndexBuffer(slot.buffer);
    slots_.clear();
}

bool GeomFarLod::Ready() const
{
    return render_lod::pixels > 0.f && lods_.has_value();
}

void GeomFarLod::OnFrameBegin(const storm::render::FrameContext &)
{
    if (render_lod::pixels <= 0.f)
    {
        if (job_)
        {
            spdlog::warn("far LOD: switched off while preparing {}: its source is dropped, it is drawn whole until it is "
                         "loaded again", name_);
            ticket_.reset();
            if (posted_)
                --render_lod::pending;
            posted_ = false;
            job_.reset();
        }
        return;
    }
    if (!job_)
        return;
    switch (job_->state.load())
    {
    case 0:
        return;
    case 3:
        spdlog::info("far LOD: making the LODs of {} stopped with the background work (the device was let go): "
                     "queued again", name_);
        --render_lod::pending;
        posted_ = false;
        job_->state = 0;
        try
        {
            post();
        }
        catch (const std::exception &error)
        {
            spdlog::error("far LOD: reposting {} failed ({}): it is drawn whole", name_, error.what());
            job_.reset();
        }
        return;
    default:
        try
        {
            install();
        }
        catch (const std::exception &error)
        {
            releaseBuffers();
            spdlog::error("far LOD: installing {} failed ({}): it is drawn whole", name_, error.what());
        }
    }
}

void GeomFarLod::install()
{
    std::shared_ptr<Job> job = std::move(job_);
    ticket_.reset();
    --render_lod::pending;
    posted_ = false;
    if (job->state == 2)
    {
        spdlog::error("far LOD: making the LODs of {} failed ({}): it is drawn whole", name_, job->failure);
        return;
    }
    far_lod::Lods &lods = *job->lods;
    const size_t bytes = lods.indices.size() * sizeof(uint16_t);
    static_buffer_ = srv_.CreateIndexBuffer(static_cast<int32_t>(bytes), false);
    auto *target = static_cast<uint16_t *>(srv_.LockIndexBuffer(static_buffer_));
    if (target == nullptr)
    {
        spdlog::error("far LOD: no index buffer of {} bytes for {}: it is drawn whole", bytes, name_);
        releaseBuffers();
        return;
    }
    std::memcpy(target, lods.indices.data(), bytes);
    srv_.UnlockIndexBuffer(static_buffer_);
    slots_.resize(lods.objects.size());
    for (size_t o = 0; o < lods.objects.size(); ++o)
    {
        const auto &object = lods.objects[o];
        for (Slot &slot : slots_[o])
        {
            slot.levels.resize(object.cluster_count, 0);
            if (object.cluster_count <= 1)
                continue;
            slot.order.resize(object.cluster_count);
            slot.offset.resize(object.cluster_count);
            slot.distance.resize(object.cluster_count);
            for (uint32_t c = 0; c < object.cluster_count; ++c)
                slot.capacity += lods.clusters[object.first_cluster + c].count[0];
            slot.buffer = srv_.CreateIndexBuffer(static_cast<int32_t>(slot.capacity * 3 * sizeof(uint16_t)), false);
            if (slot.buffer == -1)
            {
                spdlog::error("far LOD: no composed index buffer for {}: this object/pass is drawn whole", name_);
                slot.failed = true;
            }
        }
    }
    std::array<uint64_t, far_lod::kLevels> kept{};
    for (const far_lod::Cluster &cluster : lods.clusters)
        for (int l = 0; l < far_lod::kLevels; ++l)
            kept[l] += cluster.count[l];
    std::string levels;
    for (int l = 1; l < far_lod::kLevels; ++l)
        levels += fmt::format(" {:.0f}%", 100.0 * kept[l] / std::max<uint64_t>(1, kept[0]));
    // Made: once per model and game data (info); read back from the cache on every later load (debug).
    spdlog::log(job->from_cache ? spdlog::level::debug : spdlog::level::info,
                "far LOD: {}: {} clusters, {} triangles, levels{} ({} in {:.0f} ms)", name_, lods.clusters.size(),
                kept[0], levels, job->from_cache ? "read from the cache" : "made", job->ms);
    lods_ = std::move(lods);
}

bool GeomFarLod::choose(Slot &slot, uint32_t first_cluster, uint32_t count, const render_lod::View &view) const
{
    const float limit = render_lod::pixels;
    bool changed = false;
    for (uint32_t c = 0; c < count; ++c)
    {
        const far_lod::Cluster &cluster = lods_->clusters[first_cluster + c];
        float per_metre = view.pixels_per_unit; // pixels of one metre of error
        if (!view.ortho)
        {
            const float dx = cluster.center[0] - view.camera[0], dy = cluster.center[1] - view.camera[1],
                        dz = cluster.center[2] - view.camera[2];
            const float distance = (std::sqrt(dx * dx + dy * dy + dz * dz) - cluster.radius) * view.scale;
            // Not clamped to the near plane: an island's depth slices move it, and the choice must be the same in
            // all of them (one composed list serves every slice).
            per_metre /= std::max(distance, 0.01f);
        }
        const auto error = [&](int l) { return cluster.error[l] * view.scale * per_metre; };
        int level = slot.levels[c];
        while (level > 0 && error(level) > limit)
            --level;
        while (level + 1 < far_lod::kLevels && error(level + 1) * kHysteresis < limit)
            ++level;
        if (level != slot.levels[c])
        {
            changed = true;
            slot.levels[c] = static_cast<uint8_t>(level);
        }
    }
    return changed;
}

bool GeomFarLod::compose(Slot &slot, uint32_t first_cluster, uint32_t count)
{
    const render_lod::View &view = render_lod::view;
    // Nearest first: a depth slice draws one run of them (Draw).
    std::iota(slot.order.begin(), slot.order.end(), 0u);
    auto &distance = slot.distance;
    for (uint32_t c = 0; c < count; ++c)
    {
        const far_lod::Cluster &cluster = lods_->clusters[first_cluster + c];
        const float dx = cluster.center[0] - view.camera[0], dy = cluster.center[1] - view.camera[1],
                    dz = cluster.center[2] - view.camera[2];
        distance[c] = dx * dx + dy * dy + dz * dz;
    }
    std::ranges::sort(slot.order, [&](uint32_t a, uint32_t b) { return distance[a] < distance[b]; });
    auto *target = static_cast<uint16_t *>(srv_.LockIndexBuffer(slot.buffer));
    if (target == nullptr)
    {
        // Out of index buffers: this object of this pass is drawn whole from now on.
        spdlog::error("far LOD: no index buffer of {} triangles for an object of {}: that object is drawn whole",
                      slot.capacity, name_);
        slot.failed = true;
        return false;
    }
    slot.triangles = 0;
    for (const uint32_t c : slot.order)
    {
        const far_lod::Cluster &cluster = lods_->clusters[first_cluster + c];
        const int level = slot.levels[c];
        slot.offset[c] = slot.triangles;
        std::memcpy(target + 3 * size_t(slot.triangles), lods_->indices.data() + 3 * size_t(cluster.first[level]),
                    3 * sizeof(uint16_t) * cluster.count[level]);
        slot.triangles += cluster.count[level];
    }
    srv_.UnlockIndexBuffer(slot.buffer);
    slot.composed = true;
    ++render_lod::counters.composes;
    render_lod::counters.compose_bytes += 3 * sizeof(uint16_t) * size_t(slot.triangles);
    return true;
}

bool GeomFarLod::Draw(int32_t o, const GEOS::OBJECT &object, int32_t stride)
{
    const render_lod::View &view = render_lod::view;
    if (render_lod::pixels <= 0.f || !view.valid || !lods_)
        return false;
    const far_lod::ObjectLods &lod = lods_->objects[o];
    if (lod.cluster_count == 0)
        return false;
    const int pass = render_lod::pass;
    Slot &slot = slots_[o][pass];
    uint32_t first = 0, triangles = 0;
    GEOS::ID buffer = static_buffer_;
    if (lod.cluster_count == 1)
    {
        choose(slot, lod.first_cluster, 1, view);
        const far_lod::Cluster &cluster = lods_->clusters[lod.first_cluster];
        first = cluster.first[slot.levels[0]];
        triangles = cluster.count[slot.levels[0]];
    }
    else
    {
        if (slot.failed)
            return false;
        // A rewrite of the list mid-frame is safe: a draw from a buffer written this frame takes its indices as they
        // are at that draw (the shim copies them into the frame's transient indices).
        if ((choose(slot, lod.first_cluster, lod.cluster_count, view) || !slot.composed) &&
            !compose(slot, lod.first_cluster, lod.cluster_count))
            return false;
        buffer = slot.buffer;
        // Only the clusters that reach into the depth range drawn (the island's depth slices): one run of the
        // nearest-first order covers them all.
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t k = 0; k < lod.cluster_count; ++k)
        {
            const uint32_t c = slot.order[k];
            const far_lod::Cluster &cluster = lods_->clusters[lod.first_cluster + c];
            if (!view.ortho)
            {
                const float depth = ((cluster.center[0] - view.camera[0]) * view.forward[0] +
                                     (cluster.center[1] - view.camera[1]) * view.forward[1] +
                                     (cluster.center[2] - view.camera[2]) * view.forward[2]) *
                                    view.scale;
                const float radius = cluster.radius * view.scale;
                if (depth + radius < view.near_plane || depth - radius > view.far_plane)
                    continue;
            }
            lo = std::min(lo, k), hi = std::max(hi, k);
        }
        if (lo == UINT32_MAX)
        {
            render_lod::counters.full_triangles[pass] += object.ntriangles;
            return true; // nothing of it in this depth range
        }
        const uint32_t last = slot.order[hi];
        first = slot.offset[slot.order[lo]];
        triangles = slot.offset[last] + lods_->clusters[lod.first_cluster + last].count[slot.levels[last]] - first;
    }
    render_lod::counters.full_triangles[pass] += object.ntriangles;
    render_lod::counters.drawn_triangles[pass] += triangles;
    if (triangles == 0)
        return true;
    srv_.SetIndexBuffer(buffer);
    srv_.DrawIndexedPrimitive(object.start_vertex, object.num_vertices, stride, static_cast<int32_t>(first * 3),
                              static_cast<int32_t>(triangles));
    return true;
}
