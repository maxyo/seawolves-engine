#include "storm/fx/sun_fit.hpp"

#include <bvh/v2/bvh.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/node.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace storm::fx
{

namespace
{

Vec3 Direction(float azimuth_deg, float elevation_deg)
{
    const float a = azimuth_deg * 0.017453293f, e = elevation_deg * 0.017453293f;
    return {std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)};
}

} // namespace

// The hierarchy (two-child nodes over the triangles' boxes, built by the library's sweep-SAH builder and its
// reinsertion pass) and the triangles in its leaf order, each with the number it has in the place's geometry and,
// where its texture cuts holes, the slot of its texture coordinates.
struct Occluders::Tree
{
    using Node = bvh::v2::Node<float, 3>;
    using Tri = bvh::v2::PrecomputedTri<float>;
    struct CutTriangle
    {
        std::shared_ptr<const AlphaMask> mask;
        float reference = 0.5f;
        std::array<std::array<float, 2>, 3> uv{};
    };
    static constexpr uint32_t kSolid = 0xffffffffu;
    bvh::v2::Bvh<Node> bvh;
    std::vector<Tri> triangles;       // leaf order
    std::vector<uint32_t> number;     // leaf order: the triangle's number in the geometry
    std::vector<uint32_t> cut_slot;   // leaf order: into `cut`, or kSolid (empty: nothing is cut)
    std::vector<CutTriangle> cut;

    template <bool Any>
    bool Trace(const Vec3 &origin, const Vec3 &dir, float reach, std::span<const uint32_t> ignore, Hit &best) const
    {
        using Vec = bvh::v2::Vec<float, 3>;
        bvh::v2::Ray<float, 3> ray(Vec(origin[0], origin[1], origin[2]), Vec(dir[0], dir[1], dir[2]), 1e-3f, reach);
        // A stack that grows with the tree's depth, kept per thread: no allocation per ray.
        thread_local bvh::v2::GrowingStack<bvh::v2::Bvh<Node>::Index> stack;
        stack.elems.clear();
        bool found = false;
        bvh.intersect<Any, false>(ray, bvh.get_root().index, stack, [&](size_t begin, size_t end) {
            bool hit_here = false;
            for (size_t i = begin; i < end; ++i)
            {
                const auto hit = triangles[i].intersect(ray);
                if (!hit)
                    continue;
                const auto [t, u, v] = *hit;
                if (!(t < ray.tmax)) // at the reach itself: beyond
                    continue;
                if (!cut_slot.empty() && cut_slot[i] != kSolid)
                {
                    // Through the hole the texture's alpha cuts there?
                    const CutTriangle &c = cut[cut_slot[i]];
                    const float w = 1.0f - u - v;
                    if (c.mask->At(w * c.uv[0][0] + u * c.uv[1][0] + v * c.uv[2][0],
                                   w * c.uv[0][1] + u * c.uv[1][1] + v * c.uv[2][1]) <= c.reference)
                        continue;
                }
                if (!ignore.empty() && std::binary_search(ignore.begin(), ignore.end(), number[i]))
                    continue;
                ray.tmax = t;
                best = {t, number[i], u, v};
                hit_here = found = true;
                if (Any)
                    return true;
            }
            return hit_here;
        });
        return found;
    }
};

Occluders::Occluders(std::span<const Vec3> positions, std::span<const uint32_t> indices) : Occluders(positions, indices, {}, {})
{
}

Occluders::Occluders(std::span<const Vec3> positions, std::span<const uint32_t> indices,
                     std::span<const std::array<float, 2>> uvs, std::span<const Cut> cut)
    : tree_(std::make_unique<Tree>())
{
    using Vec = bvh::v2::Vec<float, 3>;
    using BBox = bvh::v2::BBox<float, 3>;
    triangles_ = indices.size() / 3;
    if (triangles_ == 0)
        return;
    // Triangles without an area (and with corners that are no numbers) are hit by nothing: left out of the tree, the
    // others keep their numbers.
    std::vector<uint32_t> kept;
    std::vector<BBox> boxes;
    std::vector<Vec> centres;
    kept.reserve(triangles_);
    boxes.reserve(triangles_);
    centres.reserve(triangles_);
    auto corner = [&](size_t t, int k) {
        const Vec3 &p = positions[indices[3 * t + k]];
        return Vec(p[0], p[1], p[2]);
    };
    for (size_t t = 0; t < triangles_; ++t)
    {
        if (indices[3 * t] >= positions.size() || indices[3 * t + 1] >= positions.size() || indices[3 * t + 2] >= positions.size())
            continue;
        const bvh::v2::Tri<float, 3> tri(corner(t, 0), corner(t, 1), corner(t, 2));
        const Vec n = cross(tri.p1 - tri.p0, tri.p2 - tri.p0);
        if (!(dot(n, n) > 1e-18f))
            continue;
        kept.push_back(static_cast<uint32_t>(t));
        boxes.push_back(tri.get_bbox());
        centres.push_back(tri.get_center());
    }
    if (kept.empty())
    {
        triangles_ = 0;
        return;
    }
    bvh::v2::DefaultBuilder<Tree::Node>::Config config;
    config.quality = bvh::v2::DefaultBuilder<Tree::Node>::Quality::High;
    tree_->bvh = bvh::v2::DefaultBuilder<Tree::Node>::build(boxes, centres, config);
    const bool cuts = uvs.size() == positions.size() && cut.size() == triangles_ &&
                      std::any_of(cut.begin(), cut.end(), [](const Cut &c) { return c.mask != nullptr; });
    tree_->triangles.resize(kept.size());
    tree_->number.resize(kept.size());
    if (cuts)
        tree_->cut_slot.assign(kept.size(), Tree::kSolid);
    for (size_t i = 0; i < kept.size(); ++i)
    {
        const size_t t = kept[tree_->bvh.prim_ids[i]];
        tree_->triangles[i] = Tree::Tri(corner(t, 0), corner(t, 1), corner(t, 2));
        tree_->number[i] = static_cast<uint32_t>(t);
        if (cuts && cut[t].mask)
        {
            tree_->cut_slot[i] = static_cast<uint32_t>(tree_->cut.size());
            tree_->cut.push_back({cut[t].mask, cut[t].reference,
                                  {uvs[indices[3 * t]], uvs[indices[3 * t + 1]], uvs[indices[3 * t + 2]]}});
        }
    }
    std::vector<size_t>().swap(tree_->bvh.prim_ids); // the triangles are in leaf order
}

Occluders::~Occluders() = default;

size_t Occluders::Bytes() const
{
    const Tree &t = *tree_;
    return t.bvh.nodes.capacity() * sizeof(Tree::Node) + t.triangles.capacity() * sizeof(Tree::Tri) +
           (t.number.capacity() + t.cut_slot.capacity()) * sizeof(uint32_t) + t.cut.capacity() * sizeof(Tree::CutTriangle);
}

bool Occluders::Occluded(const Vec3 &origin, const Vec3 &dir, float reach, std::span<const uint32_t> ignore) const
{
    Hit hit;
    return triangles_ != 0 && tree_->Trace<true>(origin, dir, reach, ignore, hit);
}

std::optional<Occluders::Hit> Occluders::Closest(const Vec3 &origin, const Vec3 &dir, float reach) const
{
    Hit hit;
    if (triangles_ == 0 || !tree_->Trace<false>(origin, dir, reach, {}, hit))
        return std::nullopt;
    return hit;
}

namespace
{

SunFit Rate(const Occluders &occluders, std::span<const SunFitSample> samples, const Vec3 &dir, float reach)
{
    SunFit fit;
    fit.dir = dir;
    if (samples.size() < 20)
        return fit;
    double mean = 0.0;
    for (const SunFitSample &s : samples)
        mean += s.luminance;
    mean /= static_cast<double>(samples.size());
    double variance = 0.0;
    for (const SunFitSample &s : samples)
        variance += (s.luminance - mean) * (s.luminance - mean);
    variance /= static_cast<double>(samples.size());
    if (variance <= 1e-8)
        return fit;
    double in_shadow = 0.0, sum_shadow = 0.0, sum_sun = 0.0;
    for (const SunFitSample &s : samples)
    {
        // A few centimetres above the ground: the ground itself is no occluder.
        if (occluders.Occluded({s.position[0], s.position[1] + 0.05f, s.position[2]}, dir, reach))
        {
            in_shadow += 1.0;
            sum_shadow += s.luminance;
        }
        else
            sum_sun += s.luminance;
    }
    const double n = static_cast<double>(samples.size()), in_sun = n - in_shadow;
    fit.shadowed = static_cast<float>(in_shadow / n);
    if (in_shadow < 5.0 || in_sun < 5.0)
        return fit;
    fit.r = static_cast<float>((sum_sun / in_sun - sum_shadow / in_shadow) * std::sqrt(in_shadow * in_sun) / (n * std::sqrt(variance)));
    fit.valid = fit.r >= kSunFitMinR;
    return fit;
}

struct Candidate
{
    float az, el;
};

constexpr size_t kCoarseStride = 4;       // the coarse ranking's samples: every 4th
constexpr size_t kCoarseMinSamples = 100; // ... when that leaves at least this many
constexpr size_t kCoarseKept = 8;         // the coarse directions rated on all samples

// Every one of `candidates` rated on `threads` threads.
std::vector<SunFit> RateAll(const Occluders &occluders, std::span<const SunFitSample> samples, float reach,
                            const std::vector<Candidate> &candidates, unsigned threads)
{
    std::vector<SunFit> rated(candidates.size());
    ParallelRanges(
        candidates.size(), threads,
        [&](size_t first, size_t last) {
            for (size_t i = first; i < last; ++i)
                rated[i] = Rate(occluders, samples, Direction(candidates[i].az, candidates[i].el), reach);
        },
        1);
    return rated;
}

// The best of `candidates`: the first of the best, as one thread would find it.
std::pair<SunFit, Candidate> Best(const Occluders &occluders, std::span<const SunFitSample> samples, float reach,
                                  const std::vector<Candidate> &candidates, unsigned threads)
{
    const std::vector<SunFit> rated = RateAll(occluders, samples, reach, candidates, threads);
    std::pair<SunFit, Candidate> best{SunFit{}, Candidate{0.0f, 45.0f}};
    for (size_t i = 0; i < candidates.size(); ++i)
        if (rated[i].r > best.first.r)
            best = {rated[i], candidates[i]};
    return best;
}

} // namespace

// The samples open to the sky: ground under a roof (inside a house, under an arcade) is dark for every direction
// and would favour low suns that shade most of the ground.
std::vector<SunFitSample> OpenSky(const Occluders &occluders, std::span<const SunFitSample> samples, float reach)
{
    std::vector<SunFitSample> open;
    open.reserve(samples.size());
    for (const SunFitSample &s : samples)
        if (!occluders.Occluded({s.position[0], s.position[1] + 0.05f, s.position[2]}, {0.0f, 1.0f, 0.0f}, reach))
            open.push_back(s);
    return open;
}

SunFit FitBakedSun(const Occluders &occluders, std::span<const SunFitSample> all, float reach, unsigned threads)
{
    const std::vector<SunFitSample> open = OpenSky(occluders, all, reach);
    const std::span<const SunFitSample> samples(open);
    // Coarse: every 8° of azimuth, 6° of elevation from 8° up to 86°; then 1° steps around the best.
    std::vector<Candidate> coarse;
    for (float el = 8.0f; el <= 86.0f; el += 6.0f)
        for (float az = 0.0f; az < 360.0f; az += 8.0f)
            coarse.push_back({az, el});
    // The coarse directions are ranked on every kCoarseStride-th sample (most of the fit's rays went there) and only
    // the kCoarseKept best are rated on all of them: the coarse best is the same as rating all on all samples
    // (the 30 towns of TEHO that bake a town light) with 0.45 instead of 1.08 million rays in a town.
    if (samples.size() >= kCoarseStride * kCoarseMinSamples)
    {
        std::vector<SunFitSample> spread;
        for (size_t i = 0; i < samples.size(); i += kCoarseStride)
            spread.push_back(samples[i]);
        const std::vector<SunFit> rough = RateAll(occluders, spread, reach, coarse, threads);
        std::vector<size_t> order(coarse.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(kCoarseKept), order.end(),
                          [&](size_t a, size_t b) { return rough[a].r > rough[b].r || (rough[a].r == rough[b].r && a < b); });
        order.resize(kCoarseKept);
        std::sort(order.begin(), order.end()); // in the coarse order: Best keeps the first of equals
        std::vector<Candidate> kept;
        for (const size_t i : order)
            kept.push_back(coarse[i]);
        coarse.swap(kept);
    }
    const auto [first, at] = Best(occluders, samples, reach, coarse, threads);
    std::vector<Candidate> fine;
    for (float el = at.el - 5.0f; el <= at.el + 5.0f; el += 1.0f)
        for (float az = at.az - 7.0f; az <= at.az + 7.0f; az += 1.0f)
            if (el >= 3.0f && el <= 89.0f)
                fine.push_back({az, el});
    SunFit best = Best(occluders, samples, reach, fine, threads).first;
    if (first.r > best.r)
        best = first;
    best.valid = best.r >= kSunFitMinR;
    return best;
}

} // namespace storm::fx
