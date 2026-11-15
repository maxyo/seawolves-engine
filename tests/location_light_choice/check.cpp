// The lights of a model in a location (Lights::SetLightsAt -> storm::location::ChooseLights): chosen where the model
// is, whatever the camera does, and without a jump while it walks. The room below is a tavern like the game's:
// candlesticks of three candles on the tables and along the walls, a fireplace.
#include "light_choice.hpp"
#include "light_components.hpp"
#include "fixture_rooms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
struct P
{
    float x, y, z;
};
struct Lamp
{
    P pos;
    float luminance, att0, att2, range;
};

int failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                           \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

std::vector<Lamp> Tavern()
{
    std::vector<Lamp> lamps;
    const auto candlestick = [&](float x, float y, float z) {
        for (const float d : {-0.12f, 0.0f, 0.12f})
            lamps.push_back({{x + d, y, z - d}, 1.6f, 0.5f, 1.5f, 15.0f});
    };
    for (float x = -6.0f; x <= 6.0f; x += 3.0f) // tables
        candlestick(x, 1.0f, 1.5f);
    for (float x = -7.0f; x <= 7.0f; x += 3.5f) // sconces of both long walls
    {
        candlestick(x, 2.4f, -4.9f);
        candlestick(x, 2.4f, 4.9f);
    }
    lamps.push_back({{-7.8f, 0.6f, 0.0f}, 1.44f, 1.5f, 0.2f, 40.0f}); // fireplace
    return lamps;
}

float Distance(P a, P b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

std::vector<uint32_t> Choose(const std::vector<Lamp> &lamps, P at, size_t budget = 7)
{
    return storm::location::ChooseLights(
        lamps.size(),
        [&](size_t i) {
            return storm::location::LightStrength(lamps[i].luminance, lamps[i].att0, 0.0f, lamps[i].att2, lamps[i].range,
                                                  Distance(lamps[i].pos, at));
        },
        budget);
}

// The lamp light a model at `at` receives: what Lights::SetLightsAt hands to the fixed-function pipeline.
float LightAt(const std::vector<Lamp> &lamps, P at)
{
    float sum = 0.0f;
    for (const uint32_t i : Choose(lamps, at))
        sum += storm::location::LightStrength(lamps[i].luminance, lamps[i].att0, 0.0f, lamps[i].att2, lamps[i].range,
                                              Distance(lamps[i].pos, at));
    return sum;
}

void FixtureComponents()
{
    using namespace storm::location;
    const auto rooms = fixture_rooms::Rooms();
    for (const auto &room : rooms)
    {
        std::vector<ComponentSource> sources;
        for (size_t i = 0; i < room.candles.size(); ++i)
            sources.push_back({i, room.candles[i].position, {2.2f, 1.5f, 0.5f}, {2.2f, 1.5f, 0.5f},
                               15.0f, {0.5f, 0.0f, 1.5f}, false});
        CHECK(BuildLightComponents(sources, 0.35f).size() == room.old_count);
        const auto components = BuildLightComponents(sources);
        CHECK(components.size() == room.count);
        size_t represented = 0;
        for (const auto &g : components)
            for (const size_t i : g.members)
            {
                CHECK(g.id == room.candles[i].component); // complete membership, including all ring layers
                ++represented;
            }
        CHECK(represented == sources.size());
        std::printf("fixture components: %s %zu -> %zu (old radius: %zu)\n",
                    room.name, sources.size(), components.size(), room.old_count);
    }
    // Negative controls of the mixed room: the nearby candles are farther than the radius from every ring candle,
    // with the same attenuation and range, so distance alone keeps these independent fixtures separate.
    const auto &mixed = rooms[2].candles;
    for (size_t i = 0; i < 16; ++i)
        for (size_t j : {size_t{16}, size_t{17}})
            CHECK(std::sqrt(LightDistanceSquared(mixed[i].position, mixed[j].position)) > 1.8);
}

void Components()
{
    using namespace storm::location;
    const auto source = [](uint64_t id, LightVector position) {
        return ComponentSource{id, position, {2.2f, 1.5f, 0.5f}, {2.2f, 1.5f, 0.5f},
                               15.0f, {0.5f, 0.0f, 1.5f}, false};
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-5; };
    CHECK(BuildLightComponents({}).empty());
    const auto one = source(17, {1.234567f, 2.5f, -3.7f});
    const auto singleton = BuildLightComponents(std::span(&one, 1));
    CHECK(singleton.size() == 1 && singleton[0].position == one.position && singleton[0].color == one.color &&
          singleton[0].steady == one.steady && singleton[0].range == one.range && singleton[0].attenuation == one.attenuation);
    std::vector<ComponentSource> boundary{source(0, {0, 0, 0}), source(1, {kLightComponentRadius, 0, 0})};
    CHECK(BuildLightComponents(boundary).size() == 1);
    boundary[1].position[0] = std::nextafter(kLightComponentRadius, 1.0f);
    CHECK(BuildLightComponents(boundary).size() == 2);

    // A three-candle holder: adjacent candles 0.30/0.29 m apart, the ends 0.46 m (farther than the old 0.35 m radius).
    std::vector<ComponentSource> candle{
        source(10, {0.0f, 2.95f, 0.0f}),
        source(11, {0.2f, 3.15f, 0.1f}),
        source(12, {0.35f, 3.0f, 0.3f})};
    CHECK(std::sqrt(LightDistanceSquared(candle.front().position, candle.back().position)) > 0.35f);
    const auto grouped = BuildLightComponents(candle);
    CHECK(grouped.size() == 1 && grouped[0].members.size() == 3);
    CHECK(close(grouped[0].color[0], 6.6) && close(grouped[0].color[1], 4.5) && close(grouped[0].color[2], 1.5));
    // Every input permutation has the same centroid, shape, RGB and stable identity; members still address its own input.
    std::vector<int> perm{0, 1, 2};
    do
    {
        const std::vector<ComponentSource> shuffled{candle[perm[0]], candle[perm[1]], candle[perm[2]]};
        const auto g = BuildLightComponents(shuffled);
        CHECK(g.size() == 1 && g[0].id == grouped[0].id && g[0].position == grouped[0].position &&
              g[0].range == grouped[0].range && g[0].color == grouped[0].color);
        for (size_t j = 0; j < 3; ++j)
            CHECK(shuffled[g[0].members[j]].id == candle[j].id);
    } while (std::next_permutation(perm.begin(), perm.end()));
    auto flicker = candle;
    flicker[0].color = {0, 0, 0};
    flicker[1].color = {4.4f, 3.0f, 1.0f};
    const auto flickered = BuildLightComponents(flicker);
    CHECK(flickered[0].position == grouped[0].position && flickered[0].id == grouped[0].id &&
          flickered[0].range == grouped[0].range && flickered[0].steady == grouped[0].steady &&
          flickered[0].attenuation == grouped[0].attenuation);
    for (auto &s : flicker) s.color = {0, 0, 0};
    CHECK(BuildLightComponents(flicker)[0].members.size() == 3); // dark flicker does not destroy shadow identity

    // Exact sum at coincident receivers for every RGB channel, with equal attenuation; no coefficient averaging.
    auto coincident = candle;
    for (auto &s : coincident) s.position = {0, 0, 0};
    coincident[0].steady = coincident[0].color = {0.3f, 1.5f, 0.7f};
    const auto merged = BuildLightComponents(coincident);
    for (const float distance : {0.1f, 0.5f, 2.0f, 8.0f, 14.0f})
        for (int k = 0; k < 3; ++k)
        {
            double expected = 0;
            for (const auto &s : coincident)
                expected += LightStrength(s.color[k], s.attenuation[0], s.attenuation[1], s.attenuation[2], s.range, distance);
            CHECK(close(expected, LightStrength(merged[0].color[k], merged[0].attenuation[0], merged[0].attenuation[1],
                                                merged[0].attenuation[2], merged[0].range, distance)));
        }
    auto mixed = coincident;
    mixed[1].attenuation = {0.1f, 3.0f, 0.3f};
    mixed[2].range = 5.0f;
    const auto split = BuildLightComponents(mixed);
    CHECK(split.size() == 3);
    for (const float distance : {0.1f, 0.5f, 2.0f, 8.0f, 14.0f})
    {
        double old_sum = 0, new_sum = 0;
        for (const auto &s : mixed) old_sum += LightStrength(float(LightLuminance(s.color)), s.attenuation[0], s.attenuation[1], s.attenuation[2], s.range, distance);
        for (const auto &g : split) new_sum += LightStrength(float(LightLuminance(g.color)), g.attenuation[0], g.attenuation[1], g.attenuation[2], g.range, distance);
        CHECK(close(old_sum, new_sum));
    }
    // Spatial collapse is an approximation, not equality of irradiance everywhere. Bound the error for the actual
    // 0.46 m holder from receivers >=1m away, six directions, all tested distances safely inside the original range.
    double largest_error = 0;
    for (const float distance : {1.0f, 2.0f, 5.0f, 10.0f})
        for (int axis = 0; axis < 3; ++axis)
            for (float direction : {-1.0f, 1.0f})
            {
                auto at = grouped[0].position; at[axis] += distance * direction;
                double original = 0;
                for (const auto &s : candle)
                    original += LightStrength(float(LightLuminance(s.color)), s.attenuation[0], s.attenuation[1], s.attenuation[2], s.range,
                                              float(std::sqrt(LightDistanceSquared(at, s.position))));
                const auto &g = grouped[0];
                const double effective = LightStrength(float(LightLuminance(g.color)), g.attenuation[0], g.attenuation[1], g.attenuation[2], g.range, distance);
                largest_error = std::max(largest_error, std::abs(effective / original - 1.0));
            }
    CHECK(largest_error < 0.08);
    std::printf("components: three-candle holder common-type receiver error max %.6f %% at >=1m (24 controls)\n", 100 * largest_error);
    // Explicit first moment, using steady RGB luminance; instantaneous RGB cannot move it.
    auto weighted = boundary; weighted[1].position = {0.3f, 0, 0};
    weighted[0].steady = {1, 0, 0}; weighted[1].steady = {0, 1, 0};
    const auto centroid = BuildLightComponents(weighted);
    CHECK(close(centroid[0].position[0], 0.3 * .587 / (.299 + .587)));
    for (size_t i : centroid[0].members)
        CHECK(centroid[0].range + 1e-5 >= weighted[i].range + std::sqrt(LightDistanceSquared(centroid[0].position, weighted[i].position)));

    // Long chain: membership cannot be reconstructed by a radius around the centroid.
    std::vector<ComponentSource> chain;
    for (int i = 0; i < 11; ++i) chain.push_back(source(i, {0.8f * i, 0, 0}));
    const auto long_group = BuildLightComponents(chain);
    CHECK(long_group.size() == 1 && long_group[0].members.size() == 11);
    CHECK(std::sqrt(LightDistanceSquared(chain[0].position, long_group[0].position)) > 4 * kLightComponentRadius);
    chain[5].steady = {0, 0, 0}; // dead/off middle source cannot bridge, original flames/coronas are not deleted here
    CHECK(BuildLightComponents(chain).size() == 2);
    chain[5] = source(5, {100, 0, 0});
    CHECK(BuildLightComponents(chain).size() == 3);
    chain[5] = source(5, {4.0f, 0, 0}); chain[5].moving = true;
    CHECK(BuildLightComponents(chain).size() == 3); // portable never merges with baked lights
    // Removing a portable source rebases all surviving indices but leaves caller-visible IDs intact.
    struct Moving { int32_t id, light; };
    std::vector<Moving> moving{{7, 2}, {8, 4}, {9, 1}};
    RebaseMovingLights(moving, 2);
    CHECK(moving.size() == 2 && moving[0].id == 8 && moving[0].light == 3 && moving[1].id == 9 && moving[1].light == 1);
    RebaseMovingLights(moving, 1);
    CHECK(moving.size() == 1 && moving[0].id == 8 && moving[0].light == 2);

    // Budget comes AFTER grouping: the first fixture uses one of seven slots, never three.
    auto room = candle;
    for (uint64_t i = 0; i < 7; ++i) room.push_back(source(20+i, {float(i)*2, 0, 10}));
    const auto components = BuildLightComponents(room);
    const auto selected = ChooseLights(components.size(), [&](size_t i) { return float(LightLuminance(components[i].steady)); }, 7);
    CHECK(components.size() == 8 && selected.size() == 7 && components[selected[0]].members.size() == 3);
    size_t represented = 0;
    for (auto i : selected) represented += components[i].members.size();
    CHECK(represented == 9); // old ungrouped seven-slot path represented only seven of these ten flames
    // Two chandelier locators 4.9 m apart along a nave remain separate.
    CHECK(BuildLightComponents(std::vector<ComponentSource>{source(1, {0.0f, 4.56f, -5.36f}),
                                                            source(2, {0.0f, 4.56f, -0.44f})}).size() == 2);
    CHECK(BuildLightComponents(std::vector<ComponentSource>{source(1, {0,0,0}), source(2,{0.5072124f,0,0})}).size() == 1);
}
} // namespace

int main()
{
    FixtureComponents();
    Components();
    const std::vector<Lamp> lamps = Tavern();
    const P hero{-2.0f, 1.2f, 2.3f}; // a character next to a table, or a lit model of the location at its own place

    // The choice takes the model's position and the lights, nothing else: wherever the camera orbits or backs off to,
    // the model gets the same lights. (The camera is not an input; the renderer-level proof on real frames is the
    // scene study of the commit: a hero standing still keeps his light while the camera backs off.)
    const std::vector<uint32_t> chosen = Choose(lamps, hero);
    const float reference = LightAt(lamps, hero);
    CHECK(reference > 0.3f && chosen.size() == 7);
    for (int step = 0; step < 16; ++step)
        CHECK(Choose(lamps, hero) == chosen);

    // Strongest first; every light left out is no stronger than the weakest chosen one.
    const auto strength = [&](uint32_t i) {
        return storm::location::LightStrength(lamps[i].luminance, lamps[i].att0, 0.0f, lamps[i].att2, lamps[i].range,
                                              Distance(lamps[i].pos, hero));
    };
    for (size_t i = 1; i < chosen.size(); ++i)
        CHECK(strength(chosen[i - 1]) >= strength(chosen[i]));
    for (uint32_t i = 0; i < lamps.size(); ++i)
        if (std::find(chosen.begin(), chosen.end(), i) == chosen.end())
            CHECK(strength(i) <= strength(chosen.back()));

    // No jump: a model walking through the whole room (1 cm steps) never sees its lamp light change by more than
    // a few per cent of the brightest spot between two steps, although the set of seven changes many times on the way.
    float brightest = 0.0f, largest_step = 0.0f;
    int changes = 0;
    std::vector<uint32_t> last_set;
    float last = -1.0f;
    for (float x = -7.5f; x <= 7.5f; x += 0.01f)
    {
        const P at{x, 1.2f, 0.4f * std::sin(x) + 0.2f};
        const float light = LightAt(lamps, at);
        std::vector<uint32_t> set = Choose(lamps, at);
        std::sort(set.begin(), set.end());
        if (last >= 0.0f)
        {
            largest_step = std::max(largest_step, std::fabs(light - last));
            changes += set != last_set ? 1 : 0;
        }
        brightest = std::max(brightest, light);
        last = light;
        last_set = set;
    }
    CHECK(changes >= 8);
    CHECK(largest_step < 0.03f * brightest);

    // A model in the middle of a ring of equal lamps is lit by seven of them (no light is lost to a tie), in list order.
    std::vector<Lamp> ring;
    for (int i = 0; i < 9; ++i)
        ring.push_back({{std::cos(0.7f * static_cast<float>(i)) * 3.0f, 0.0f, std::sin(0.7f * static_cast<float>(i)) * 3.0f}, 1.0f, 1.0f, 0.0f, 10.0f});
    CHECK((Choose(ring, P{0.0f, 0.0f, 0.0f}) == std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6}));

    // A strong fireplace farther away beats a nearer weak candle; a lamp out of range or without light is never chosen.
    const std::vector<Lamp> mixed = {{{1.0f, 0.0f, 0.0f}, 0.05f, 0.5f, 1.5f, 15.0f},
                                     {{3.0f, 0.0f, 0.0f}, 1.44f, 1.5f, 0.2f, 40.0f},
                                     {{2.0f, 0.0f, 0.0f}, 5.0f, 0.5f, 1.5f, 1.0f},
                                     {{0.5f, 0.0f, 0.0f}, 0.0f, 0.5f, 1.5f, 15.0f}};
    CHECK((Choose(mixed, P{0.0f, 0.0f, 0.0f}, 1) == std::vector<uint32_t>{1}));
    CHECK((Choose(mixed, P{0.0f, 0.0f, 0.0f}) == std::vector<uint32_t>{1, 0}));

    // Fewer lights than the budget: all of them. None: nothing.
    CHECK(Choose(std::vector<Lamp>(lamps.begin(), lamps.begin() + 4), hero).size() == 4);
    CHECK(Choose({}, hero).empty());

    if (failures)
        return 1;
    std::printf("PASS: a model's lights follow its own position (largest step %.2f %% of the brightest spot over %d set changes)\n",
                100.0f * largest_step / brightest, changes);
    return 0;
}
