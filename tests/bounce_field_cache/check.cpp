#include "storm/fx/bounce_field_cache.hpp"

#include <cassert>
#include <iostream>
#include <limits>

using namespace storm::fx;
namespace cache = storm::fx::bounce_cache;

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path directory = argv[1]; // runner-owned temporary, never a user's cache
    BounceScene scene;
    scene.grid = {{2, 2, 2}, {-1, -1, -1}, 1};
    scene.points = scene.grid.size;
    scene.rays = 4;
    scene.position = {{0, 0, 0}, {0.5f, 0, 0}};
    scene.normal = {{0, 1, 0}, {0, 1, 0}};
    scene.albedo = {{0.6f, 0.2f, 0.1f}, {0.1f, 0.3f, 0.6f}};
    scene.hit.resize(scene.Points() * scene.rays);
    for (size_t i = 0; i < scene.hit.size(); ++i) scene.hit[i] = i % 2 ? 1 : 3;
    scene.inside.resize(scene.Points());
    scene.link.resize(scene.Points(), 7);
    std::vector<Vec3> geometry = {{-2, -1, -2}, {2, -1, -2}, {0, -1, 2}};
    std::vector<uint32_t> indices = {0, 1, 2};
    std::vector<std::array<float, 2>> uvs = {{0, 0}, {1, 0}, {0, 1}};
    auto mask = std::make_shared<AlphaMask>();
    mask->width = mask->height = 1; mask->alpha = {255};
    std::vector<Occluders::Cut> cuts = {{mask, 0.5f}};
    auto identity = [&] { return cache::SceneKey(scene, geometry, indices, uvs, cuts); };
    const auto scene_key = identity();
    auto changes = [&](auto &value, auto replacement) {
        auto old = value; value = replacement; assert(identity() != scene_key); value = std::move(old);
    };
    changes(scene.grid.cell, 2.0f); changes(scene.grid.origin[0], 3.0f); changes(scene.grid.size[0], 3);
    changes(scene.points[0], 3); changes(scene.stride, 2); changes(scene.rays, 8);
    changes(scene.position[0][0], 1.0f); changes(scene.normal[0][0], 1.0f); changes(scene.albedo[0][0], 0.8f);
    changes(scene.hit[0], uint32_t(0)); changes(scene.inside[0], uint8_t(1)); changes(scene.link[0], uint8_t(0));
    changes(geometry[0][0], -3.0f); changes(indices[0], uint32_t(1)); changes(uvs[0][0], 0.4f);
    changes(cuts[0].reference, 0.4f); changes(mask->alpha[0], uint8_t(0)); changes(mask->width, 2);
    // Equal immutable content at different addresses is the same key, including shared mask aliases.
    cuts[0].mask = std::make_shared<AlphaMask>(*mask);
    assert(identity() == scene_key);
    cuts[0].mask = mask;

    std::vector<Lamp> lamps = {{{0, 2, 0}, {1, 0.5f, 0.2f}, 6, 1, 0.4f, {}}};
    std::vector<float> holders = {0.1f};
    std::vector<std::vector<uint32_t>> fixtures = {{0}};
    auto key_for_lamps = [&] { return cache::LampKey(scene_key, lamps, holders, fixtures); };
    const auto key = *key_for_lamps();
    auto lamp_changes = [&](auto &value, auto replacement) {
        auto old = value; value = replacement; assert(key_for_lamps() != key); value = std::move(old);
    };
    lamp_changes(lamps[0].position[0], 1.0f); lamp_changes(lamps[0].color[1], 0.6f);
    lamp_changes(lamps[0].range, 7.0f); lamp_changes(lamps[0].intensity, 2.0f); lamp_changes(lamps[0].core, 0.5f);
    lamp_changes(holders[0], 0.3f); lamp_changes(fixtures[0][0], uint32_t(1));
    assert(cache::LampKey({scene_key[0] ^ 1, scene_key[1]}, lamps, holders, fixtures) != key);
    auto snapshot = std::make_shared<AggregateSnapshot>();
    snapshot->members.resize(2);
    snapshot->members[0].position = {0, 2, 0}; snapshot->members[0].color = {1, 0.5f, 0.2f};
    snapshot->members[1].position = {0.5f, 2, 0}; snapshot->members[1].color = {0.2f, 0.5f, 1};
    snapshot->groups.resize(1);
    snapshot->groups[0].count = 2;
    snapshot->groups[0].center = {0.25f, 2, 0}; snapshot->groups[0].support = 6;
    snapshot->groups[0].shape = {6, 1, 0.4f};
    lamps[0].aggregate = {snapshot, 0};
    const auto compound = *key_for_lamps();
    auto compound_changes = [&](auto &value, auto replacement) {
        auto old = value; value = replacement; assert(key_for_lamps() != compound); value = std::move(old);
    };
    compound_changes(snapshot->groups[0].center[0], 2.0f); compound_changes(snapshot->groups[0].support, 7.0f);
    compound_changes(snapshot->groups[0].shape.range, 7.0f); compound_changes(snapshot->groups[0].shape.intensity, 2.0f);
    compound_changes(snapshot->groups[0].shape.core, 0.6f); compound_changes(snapshot->members[1].color[1], 0.8f);
    compound_changes(snapshot->members[1].position[0], 2.0f);
    snapshot->groups[0].moving = true; assert(!key_for_lamps()); snapshot->groups[0].moving = false;
    snapshot->groups[0].count = 3; assert(!key_for_lamps()); snapshot->groups[0].count = 2;
    lamps[0].aggregate.group = 1; assert(!key_for_lamps()); lamps[0].aggregate.group = 0;
    lamps[0].aggregate.snapshot = std::make_shared<const AggregateSnapshot>(*snapshot);
    assert(key_for_lamps() == compound); // snapshot generation/address is not content identity
    lamps[0].aggregate = {};

    const Occluders occluders(geometry, indices, uvs, cuts);
    const auto compute = [&] {
        return GatherBounce(scene, LampsOnSurfels(occluders, scene, lamps, holders, fixtures, 1), 1);
    };
    const auto reference = compute();
    assert(reference.Energy() > 0);
    auto same = [](const BounceField &a, const BounceField &b) {
        return a.points == b.points && a.origin == b.origin && a.spacing == b.spacing && a.moments == b.moments;
    };
    auto bytes = cache::Encode(key, reference);
    assert(!bytes.empty() && bytes.size() <= cache::kMaxBytes);
    assert(same(reference, *cache::Decode(bytes, key, scene)));
    assert(!cache::Decode(bytes, {key[0], key[1] ^ 1}, scene));
    auto wrong_scene = scene; wrong_scene.grid.origin[0] += 1;
    assert(!cache::Decode(bytes, key, wrong_scene));
    for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(32), size_t(60), size_t(64), bytes.size() - 1})
    {
        auto damaged = bytes; damaged[offset] ^= 0xff;
        assert(!cache::Decode(damaged, key, scene));
    }
    auto truncated = bytes; truncated.pop_back(); assert(!cache::Decode(truncated, key, scene));
    auto trailing = bytes; trailing.push_back(0); assert(!cache::Decode(trailing, key, scene));
    auto invalid = reference; invalid.moments[0] = std::numeric_limits<float>::quiet_NaN();
    assert(cache::Encode(key, invalid).empty());
    invalid = reference; invalid.points = {std::numeric_limits<int>::max(), 2, 2};
    assert(cache::Encode(key, invalid).empty());
    invalid = reference; invalid.spacing = 0; assert(cache::Encode(key, invalid).empty());

    cache::Publication publication;
    int computes = 0;
    auto resolve = [&] {
        if (auto hit = cache::Read(directory, key, scene)) return *hit;
        ++computes;
        auto field = compute(); assert(cache::Write(directory, key, field, publication)); return field;
    };
    assert(same(resolve(), reference)); assert(computes == 1);
    assert(same(resolve(), reference)); assert(computes == 1); // real lamp/gather work skipped on a warm lookup
    // Existing final packing remains bit-identical, including directional bytes and lamp-only encoding.
    std::vector<uint8_t> a(scene.Points() * 4, 91), b = a, block_a, block_b;
    const auto restored = *cache::Read(directory, key, scene);
    EncodeBounceBlocks(scene.grid, reference, {1, 1, 1}, nullptr, a, block_a, 1);
    EncodeBounceBlocks(scene.grid, restored, {1, 1, 1}, nullptr, b, block_b, 1);
    assert(a == b && block_a == block_b);
    auto second = reference; second.moments[0] += 0.25f;
    cache::Publication canceled; canceled.cancel();
    assert(!cache::Write(directory, key, second, canceled));
    assert(same(*cache::Read(directory, key, scene), reference));
    cache::Publication during_io;
    assert(!cache::Write(directory, key, second, during_io, [&] { during_io.cancel(); }));
    assert(same(*cache::Read(directory, key, scene), reference));
    bool cancelled = false;
    try { cache::Write(directory, key, second, publication, [] { throw BakeCancelled{}; }); }
    catch (const BakeCancelled &) { cancelled = true; }
    assert(cancelled && same(*cache::Read(directory, key, scene), reference));
    assert(!std::filesystem::exists(cache::Slot(directory, key).parent_path() / "pending.tmp"));
    // Slot collision/replacement checks the COMPLETE key, never just the filename's low bits.
    for (uint64_t i = 0; i < 12; ++i)
    {
        cache::Key k{i, key[1]}; assert(cache::Write(directory, k, reference, publication));
        assert(same(*cache::Read(directory, k, scene), reference));
    }
    assert(!cache::Read(directory, {0, key[1]}, scene));
    size_t files = 0, disk = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cache::Slot(directory, key).parent_path()))
    { ++files; disk += entry.file_size(); }
    assert(files == cache::kSlots && disk <= cache::kSlots * cache::kMaxBytes);
    { std::ofstream foreign(directory / "existing-texture-cache"); foreign << "preserved"; }
    assert(!cache::Write(directory / "existing-texture-cache", key, reference, publication));
    assert(std::filesystem::file_size(directory / "existing-texture-cache") == 9);
    std::cout << "PASS: complete input keys; real field/encoded bytes exact; warm compute skipped; corrupt/bounds/"
                 "cancellation/atomic replacement/namespace controls\n";
}
