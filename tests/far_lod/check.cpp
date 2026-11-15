// Far LOD host check (run.py): the levels of synthetic models (verify.hpp says what is checked), the result
// independent of the threads, and a broken layout refused. The game's models: tools/game_checks/far_lod.
#include "verify.hpp"
#include <limits>

namespace
{
using namespace far_lod_check;

struct Builder
{
    Model model;
    std::vector<uint8_t> vertices; // one buffer, VERTEX0 (36 bytes)

    uint32_t vertex(float x, float y, float z, float u, float v)
    {
        uint8_t raw[36] = {};
        const float pos[3] = {x, y, z}, normal[3] = {0, 1, 0}, uv[2] = {u, v};
        std::memcpy(raw, pos, 12);
        std::memcpy(raw + 12, normal, 12);
        std::memcpy(raw + 28, uv, 8);
        vertices.insert(vertices.end(), raw, raw + 36);
        return static_cast<uint32_t>(vertices.size() / 36 - 1);
    }
    // A bumpy n×n grid from (x0, z0), `cell` metres per cell, as one object.
    void grid(float x0, float z0, int n, float cell, float bump)
    {
        MeshObject object;
        object.first_vertex = static_cast<uint32_t>(vertices.size() / 36);
        object.first_triangle = static_cast<uint32_t>(model.triangles.size() / 3);
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
            {
                const float x = x0 + i * cell, z = z0 + j * cell;
                vertex(x, bump * std::sin(x * 0.05f) * std::cos(z * 0.07f), z, x * 0.1f, z * 0.1f);
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const uint16_t a = static_cast<uint16_t>(j * (n + 1) + i), b = a + 1, c = a + (n + 1), d = c + 1;
                model.triangles.insert(model.triangles.end(), {a, c, b, b, c, d});
            }
        object.vertex_count = static_cast<uint32_t>(vertices.size() / 36) - object.first_vertex;
        object.triangle_count = static_cast<uint32_t>(model.triangles.size() / 3) - object.first_triangle;
        model.objects.push_back(object);
    }
    // A decal: a quad 2 mm above the grid's flat top at height `y`, after a quad of the base in the same plane.
    void decal_pair(float x, float z, float y)
    {
        MeshObject object;
        object.first_vertex = static_cast<uint32_t>(vertices.size() / 36);
        object.first_triangle = static_cast<uint32_t>(model.triangles.size() / 3);
        for (const float h : {y, y + 0.002f})
        {
            const uint16_t a = static_cast<uint16_t>(vertices.size() / 36 - object.first_vertex);
            vertex(x, h, z, 0, 0), vertex(x + 1, h, z, 1, 0), vertex(x, h, z + 1, 0, 1), vertex(x + 1, h, z + 1, 1, 1);
            model.triangles.insert(model.triangles.end(), {a, uint16_t(a + 2), uint16_t(a + 1), uint16_t(a + 1),
                                                           uint16_t(a + 2), uint16_t(a + 3)});
        }
        object.vertex_count = static_cast<uint32_t>(vertices.size() / 36) - object.first_vertex;
        object.triangle_count = static_cast<uint32_t>(model.triangles.size() / 3) - object.first_triangle;
        model.objects.push_back(object);
    }
    // A decal object of its own: a quad 2 mm above height `y` (over another object's flat surface).
    void decal(float x, float z, float y)
    {
        MeshObject object;
        object.first_vertex = static_cast<uint32_t>(vertices.size() / 36);
        object.first_triangle = static_cast<uint32_t>(model.triangles.size() / 3);
        vertex(x, y + 0.002f, z, 0, 0), vertex(x + 3, y + 0.002f, z, 1, 0), vertex(x, y + 0.002f, z + 3, 0, 1),
            vertex(x + 3, y + 0.002f, z + 3, 1, 1);
        model.triangles.insert(model.triangles.end(), {0, 2, 1, 1, 2, 3});
        object.vertex_count = 4;
        object.triangle_count = 2;
        model.objects.push_back(object);
    }
    Model done()
    {
        model.buffers = {vertices};
        model.strides = {36};
        return model;
    }
};

// A near-coplanar pair can overlap in the exact test's projection but not along another normal's dominant axis.
// Unrelated triangles in the same normal/distance bin must not change whether that pair is protected.
void depthTieProjection()
{
    for (int rotation = 0; rotation < 3; ++rotation)
    {
        Builder b;
        const auto vertex = [&](float x, float y, float z) {
            const float p[3] = {x, y, z};
            b.vertex(p[rotation], p[(rotation + 1) % 3], p[(rotation + 2) % 3], 0, 0);
        };
        // More than 32 candidates select the grid broad phase; these have dominant Z, while the target pair
        // has dominant X (before rotation). They share its plane bins, but sit far away along Y.
        for (int t = 0; t < 40; ++t)
        {
            const float y = 10.f + 3.f * t;
            vertex(0, y, 0), vertex(1.0009765625f, y, 1), vertex(0, y + 1, 0);
            const auto first = static_cast<uint16_t>(3 * t);
            b.model.triangles.insert(b.model.triangles.end(), {first, uint16_t(first + 1), uint16_t(first + 2)});
        }
        b.model.objects.push_back({0, 0, 120, 0, 40});
        vertex(0, 0, 0), vertex(0.999755859375f, 0, 1), vertex(0, 2, 0);
        vertex(-0.00390625f, 0.5f, 0.00390625f), vertex(-1.003662109375f, 0.5f, -0.99609375f),
            vertex(-0.00390625f, 0.6f, 0.00390625f);
        b.model.triangles.insert(b.model.triangles.end(), {0, 1, 2, 3, 4, 5});
        b.model.objects.push_back({0, 120, 6, 40, 2});
        const Model model = b.done();
        const auto views = model.views();
        const auto local = far_lod::DepthTiePairs(model.objects[1], views[0], model.triangles);
        const auto global = far_lod::ModelDepthTies(model.objects, views, model.triangles);
        // The target has a unique dominant axis in every rotation: every oracle must contain the pair.
        if (local != std::vector<std::pair<uint32_t, uint32_t>>{{0, 1}})
            fail("depth-tie projection fixture does not overlap");
        std::vector<std::pair<uint32_t, uint32_t>> own;
        for (const auto &[a, c] : global)
            if (a.object == 1 && c.object == 1)
                own.emplace_back(a.triangle, c.triangle);
        if (own != local)
            fail("unrelated model triangles changed a depth tie across a projection-axis boundary");
        check(model);
        // The same triangles in one object must also agree with the two-triangle exact result.
        Model merged = model;
        for (size_t i = 120; i < merged.triangles.size(); ++i)
            merged.triangles[i] += 120;
        merged.objects = {{0, 0, 126, 0, 42}};
        std::vector<std::pair<uint32_t, uint32_t>> expected;
        for (const auto &[a, c] : local)
            expected.emplace_back(a + 40, c + 40);
        if (far_lod::DepthTiePairs(merged.objects[0], merged.views()[0], merged.triangles) != expected)
            fail("unrelated object triangles changed a depth tie across a projection-axis boundary");
        check(merged);
    }
    std::printf("depth-tie projection consistency: passed\n");
}

void crossObjectDecal()
{
    // A decal object 2 mm over a grid (a depth tie across objects): both sides are kept exact, outside the
    // simplifier, and the simplified grid still meets the kept triangles (fixed/surface seams).
    Builder b;
    b.grid(0, 0, 20, 1.f, 0.f);
    b.decal(7.5f, 7.5f, 0.f);
    const Model model = b.done();
    std::set<std::pair<uint32_t, uint32_t>> exact;
    for (const auto &[a, c] : far_lod::ModelDepthTies(model.objects, model.views(), model.triangles))
        if (a.object != c.object)
            exact.insert({a.object, a.triangle}), exact.insert({c.object, c.triangle});
    // Both base triangles of cell (8,8), wholly under the decal, and both decal triangles.
    for (const auto &required : {std::pair{0u, 336u}, std::pair{0u, 337u}, std::pair{1u, 0u}, std::pair{1u, 1u}})
        if (!exact.contains(required))
            fail("cross-object depth ties missed a known base/decal triangle");
    const Totals t = check(model);
    if (t.fixed_seams == 0)
        fail("cross-object decal fixture has no fixed/simplified seam");
    if (t.kept.back() >= t.kept.front())
        fail("cross-object decal fixture did not exercise simplification");
    std::printf("cross-object decal: kept exact, sealed fixed/surface boundaries passed\n");
}

void damagedCache()
{
    Builder b;
    b.grid(0, 0, 8, 1.f, 0.f);
    b.grid(100, 0, 8, 1.f, 0.f);
    const Model model = b.done();
    const auto lods = far_lod::Build(model.objects, model.views(), model.triangles);
    const auto valid = far_lod::Encode(lods);
    if (!far_lod::Decode(valid, model.objects))
        fail("damage fixture did not round trip");
    size_t cases = 0;
    const auto reject = [&](const far_lod::Lods &broken, const char *why) {
        if (far_lod::Decode(far_lod::Encode(broken), model.objects))
            fail(std::string("Decode accepted ") + why);
        ++cases;
    };
    const auto mutate = [&](const auto &change, const char *why) {
        auto broken = lods;
        change(broken);
        reject(broken, why);
    };
    mutate([](auto &x) { x.objects[1].first_cluster = 0; }, "overlapping object cluster ownership");
    mutate([](auto &x) { x.clusters.push_back(x.clusters.back()); }, "unowned cluster");
    mutate([](auto &x) { x.clusters[0].first[0] = UINT32_MAX; }, "triangle range overflow");
    mutate([](auto &x) { x.clusters[0].count[0] = 0; }, "empty level zero");
    mutate([](auto &x) { --x.clusters[0].count[0]; }, "wrong level-zero total");
    mutate([](auto &x) { x.clusters[0].count[1] = x.clusters[0].count[0] + 1; }, "growing level");
    mutate([](auto &x) { x.clusters[0].radius = -1; }, "negative sphere radius");
    mutate([](auto &x) { x.clusters[0].center[0] = std::numeric_limits<float>::quiet_NaN(); }, "NaN center");
    mutate([](auto &x) { x.clusters[0].radius = std::numeric_limits<float>::infinity(); }, "infinite radius");
    mutate([](auto &x) { x.clusters[0].error[0] = 0.01f; }, "nonzero level-zero error");
    mutate([](auto &x) { x.clusters[0].error[1] = std::numeric_limits<float>::quiet_NaN(); }, "NaN error");
    mutate([](auto &x) { x.clusters[0].error[1] = -1; }, "negative error");
    mutate([](auto &x) { x.clusters[0].error[1] = far_lod::kLevelError[1] * 2; }, "excessive error");
    mutate([](auto &x) { x.indices[0] = UINT16_MAX; }, "out-of-object index");
    mutate([](auto &x) { x.indices.insert(x.indices.end(), {0, 1, 2}); }, "unowned indices");
    // Raw format/codec damage, in addition to structurally invalid but correctly encoded payloads above.
    for (const size_t at : {size_t(0), size_t(4), size_t(8), size_t(16), size_t(20),
                           valid.size() - 1})
    {
        auto broken = valid;
        if (at == valid.size() - 1)
            broken.pop_back();
        else
            broken[at] ^= 0xff;
        if (far_lod::Decode(broken, model.objects))
            fail("Decode accepted corrupted header or truncated codec");
        ++cases;
    }
    // The encoded stream starts after the fixed-size header, object table and cluster table.
    auto bad_codec = valid;
    bad_codec[24 + sizeof(far_lod::ObjectLods) * lods.objects.size() + sizeof(far_lod::Cluster) * lods.clusters.size()] = 0;
    if (far_lod::Decode(bad_codec, model.objects))
        fail("Decode accepted a bad index codec header");
    ++cases;
    std::printf("Decode damage: %zu structural, numeric, header and codec cases passed\n", cases);
}

void synthetic()
{
    if (!far_lod::Wanted("islands/Martinique.gm", 1.f, false) ||
        !far_lod::Wanted("SHIPS\\frigate.gm", 1.f, false) ||
        !far_lod::Wanted("locations/town.gm", 50.f, false) ||
        far_lod::Wanted("locations/cabin.gm", 49.f, false) ||
        far_lod::Wanted("ships/frigate.gm", 100.f, true) ||
        far_lod::Wanted("characters/man.gm", 100.f, false) ||
        far_lod::Wanted("ships_extra/frigate.gm", 100.f, false))
        fail("far LOD model eligibility crossed its folder, radius or animation boundary");
    {
        // Two neighbouring bumpy grids (two objects meeting along x = 100, copies of the same positions), one large
        // enough for several clusters, and a decal pair.
        Builder b;
        b.grid(0, 0, 120, 100.f / 120, 4.f);
        b.grid(100, 0, 40, 100.f / 120, 4.f);
        b.decal_pair(500, 500, 3.f);
        const Model model = b.done();
        const Totals t = check(model);
        const auto views = model.views();
        if (far_lod::Encode(far_lod::Build(model.objects, views, model.triangles)) !=
            far_lod::Encode(far_lod::Build(model.objects, views, model.triangles, threads)))
            fail("the LODs depend on the threads");
        if (t.clusters < 15)
            fail("the large grid was not split into clusters");
        if (t.kept[far_lod::kLevels - 1] > 0.5 * t.triangles)
            fail("the bumpy grids did not simplify");
        std::printf("synthetic: %zu triangles, %zu clusters, kept per level:", t.triangles, t.clusters);
        for (const double k : t.kept)
            std::printf(" %.0f", k);
        std::printf(", %zu bytes encoded\n", t.encoded);
    }
    {
        // A flat grid: every level beyond 0 collapses it to the locked outline at most; with a decal object over it
        // the grid's triangles under the decal stay (verify.hpp checks them at every level).
        Builder b;
        b.grid(0, 0, 30, 1.f, 0.f);
        b.decal(10.5f, 10.5f, -0.002f);
        const Model model = b.done();
        const auto views = model.views();
        const auto ties = far_lod::ModelDepthTies(model.objects, views, model.triangles);
        if (std::ranges::none_of(ties, [](const auto &pair) { return pair.first.object != pair.second.object; }))
            fail("the decal object does not tie with the grid under it");
        const Totals t = check(model);
        if (t.kept[1] >= t.kept[0])
            fail("a flat grid kept all its triangles at level 1");
    }
    {
        // A broken layout is refused.
        Builder b;
        b.grid(0, 0, 4, 1.f, 0.f);
        Model model = b.done();
        model.objects[0].vertex_count -= 1;
        if (!far_lod::CheckMeshLayout(model.objects, model.counts(), model.triangles))
            fail("CheckMeshLayout took an index outside its object");
    }
    {
        Model empty;
        empty.objects.push_back({UINT32_MAX, 0, 0, 0, 0});
        check(empty);
    }
    std::printf("synthetic models: passed\n");
}
} // namespace

int main()
{
    try
    {
        depthTieProjection();
        synthetic();
        crossObjectDecal();
        damagedCache();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
