#include "storm/fx/place_probes.hpp"

#include "storm/fx/bakes.hpp"
#include "storm/fx/hash.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>

#if defined(__linux__)
#include <time.h>
#endif

namespace storm::fx
{

namespace
{

constexpr float kOpenElevation = 0.258819f; // sin 15°: "open" counts the directions above this
constexpr uint32_t kFileMagic = 0x43414c50u;   // "PLAC"
constexpr uint32_t kProbesMagic = 0x42525050u; // "PPRB"

struct Reader
{
    std::span<const uint8_t> bytes;
    size_t at = 0;
    bool Take(void *out, size_t size)
    {
        if (size > bytes.size() - at)
            return false;
        std::memcpy(out, bytes.data() + at, size);
        at += size;
        return true;
    }
    template <class T> bool Take(T &value) { return Take(&value, sizeof(value)); }
};

void Put(std::vector<uint8_t> &out, const void *data, size_t size)
{
    out.insert(out.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
}
template <class T> void Put(std::vector<uint8_t> &out, const T &value)
{
    Put(out, &value, sizeof(value));
}

// The processor time the calling thread has used, in milliseconds (0 where the system has no such clock): what a
// bake costs whatever else the device is doing.
double ThreadCpuMs()
{
#if defined(__linux__)
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0)
        return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_nsec) * 1e-6;
#endif
    return 0.0;
}

uint8_t Byte(float unit)
{
    return static_cast<uint8_t>(std::lround(255.0f * std::clamp(unit, 0.0f, 1.0f)));
}

// The occlusion raised so that the floor one walks on reads as an open one (ao_volume.hpp kAoWalkLift): what the
// volume adds is local (under a table, behind a barrel, in a corner). Where the whole place is narrow or low (a ship's
// corridor, a hold between its ribs, a hut) the floor has walls within the radius everywhere and sees less than an
// open floor does: the place would go dark as a whole, though its baked colours already hold how light it is.
float LiftToWalk(AoVolume &volume, std::span<const Vec3> walk)
{
    std::vector<float> seen;
    seen.reserve(walk.size());
    for (const Vec3 &point : walk)
    {
        const Vec3 above{point[0], point[1] + volume.cell, point[2]}; // as the receivers read it
        bool within = true;
        for (int k = 0; k < 3; ++k)
            within = within && above[k] >= volume.origin[k] &&
                     above[k] < volume.origin[k] + static_cast<float>(volume.size[k]) * volume.cell;
        if (within)
            seen.push_back(volume.Sample(above, {0.0f, 1.0f, 0.0f}));
    }
    if (seen.empty())
        return 1.0f;
    const auto middle = seen.begin() + static_cast<std::ptrdiff_t>(seen.size() / 2);
    std::nth_element(seen.begin(), middle, seen.end());
    const float lift = std::clamp(kAoOpenSurface / std::max(*middle, 0.05f), 1.0f, kAoWalkLift);
    if (lift > 1.0f)
        for (uint8_t &value : volume.visibility)
            value = static_cast<uint8_t>(std::min(255.0f, std::round(static_cast<float>(value) * lift)));
    return lift;
}

// Ray `r` of the kProbeRays over the whole sphere (a spherical Fibonacci set), turned about y by `turn` radians.
Vec3 SphereDirection(int r, float turn)
{
    const float y = 1.0f - (2.0f * static_cast<float>(r) + 1.0f) / static_cast<float>(kProbeRays);
    const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y)), phi = 2.3999632f * static_cast<float>(r) + turn;
    return {radius * std::cos(phi), y, radius * std::sin(phi)};
}

// Ray `r` of the kSkyRays over the upper hemisphere, cosine-weighted around +y (a spiral): each stands for the same
// share of the sky's light.
Vec3 SkyDirection(int r, float turn)
{
    const float u = (static_cast<float>(r) + 0.5f) / static_cast<float>(kSkyRays);
    const float radius = std::sqrt(u), phi = 2.3999632f * static_cast<float>(r) + turn;
    return {radius * std::cos(phi), std::sqrt(std::max(0.0f, 1.0f - u)), radius * std::sin(phi)};
}

// The turn of a cell's rays where the cells are turned against each other (no banding between them).
float ProbeTurn(size_t cell)
{
    return static_cast<float>((cell * 2654435761u) % 1000) * 0.0062831853f;
}

// Cells whose every ray up is closed (inside the geometry) next to open ones: the most closed of those.
void CloseInside(VisibilityVolume &v, const std::vector<uint8_t> &closed)
{
    const std::vector<uint8_t> baked = v.sky_open;
    const size_t count = v.Cells();
    const int step[3] = {1, v.size[0], v.size[0] * v.size[1]};
    for (size_t c = 0; c < count; ++c)
    {
        if (!closed[c])
            continue;
        const int at[3] = {static_cast<int>(c % v.size[0]), static_cast<int>((c / v.size[0]) % v.size[1]),
                           static_cast<int>(c / (static_cast<size_t>(v.size[0]) * v.size[1]))};
        int least[2] = {256, 256};
        for (int k = 0; k < 3; ++k)
            for (int s = -1; s <= 1; s += 2)
            {
                if (at[k] + s < 0 || at[k] + s >= v.size[k])
                    continue;
                const size_t o = c + static_cast<size_t>(static_cast<std::ptrdiff_t>(s) * step[k]);
                if (closed[o])
                    continue;
                least[0] = std::min<int>(least[0], baked[o * 2]);
                least[1] = std::min<int>(least[1], baked[o * 2 + 1]);
            }
        if (least[0] < 256)
        {
            v.sky_open[c * 2] = static_cast<uint8_t>(least[0]);
            v.sky_open[c * 2 + 1] = static_cast<uint8_t>(least[1]);
        }
    }
}

} // namespace

PlaceProbes BakePlaceProbes(const Occluders &occluders, std::span<const Vec3> positions, std::span<const uint32_t> indices,
                            const ProbeParams &params)
{
    PlaceProbes probes;
    if (indices.empty() || occluders.Empty())
        return probes;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const uint32_t i : indices)
        if (i < positions.size())
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], positions[i][k]);
                hi[k] = std::max(hi[k], positions[i][k]);
            }
    const float diagonal = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                     (hi[2] - lo[2]) * (hi[2] - lo[2]));
    if (params.lo[0] <= params.hi[0])
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::max(lo[k], params.lo[k]);
            hi[k] = std::min(hi[k], params.hi[k]);
        }
    for (int k = 0; k < 3; ++k)
        if (!(lo[k] <= hi[k]))
            return probes;
    // One cell of margin on every side; the cell grows until the grid fits the budget and a 3D texture.
    float cell = std::max(params.cell, 0.05f);
    std::array<int, 3> size{};
    for (;; cell *= 1.1f)
    {
        size_t cells = 1;
        bool fits = true;
        for (int k = 0; k < 3; ++k)
        {
            size[k] = static_cast<int>(std::ceil((hi[k] - lo[k]) / cell)) + 2;
            cells *= static_cast<size_t>(size[k]);
            fits = fits && size[k] <= kProbeMaxSide;
        }
        if (fits && cells <= params.max_cells)
            break;
    }
    VisibilityVolume &v = probes.visibility;
    v.cell = cell;
    v.size = size;
    for (int k = 0; k < 3; ++k)
        v.origin[k] = lo[k] - cell;
    const size_t count = v.Cells();
    v.sky_open.assign(count * 2, 0);
    const bool near = params.near_sky > 0.0f, ao = params.ao_radius > 0.0f;
    if (near)
        probes.near_sky.assign(count, 0);
    if (ao)
    {
        probes.ao.size = size;
        probes.ao.origin = v.origin;
        probes.ao.cell = cell;
        probes.ao.visibility.assign(count * kAoLobes, 255);
    }
    const float reach = std::min(params.reach, diagonal + cell);
    std::vector<uint8_t> closed(count, 0);
    std::atomic<uint64_t> rays{0}, cpu_us{0};
    ParallelRanges(count, params.threads, [&](size_t first, size_t last) {
        const double began = ThreadCpuMs();
        uint64_t traced = 0;
        for (size_t c = first; c < last; ++c)
        {
            const int x = static_cast<int>(c % size[0]), y = static_cast<int>((c / size[0]) % size[1]),
                      z = static_cast<int>(c / (static_cast<size_t>(size[0]) * size[1]));
            const Vec3 o{v.origin[0] + (x + 0.5f) * cell, v.origin[1] + (y + 0.5f) * cell, v.origin[2] + (z + 0.5f) * cell};
            const float turn = params.turned ? ProbeTurn(c) : 0.0f;
            float up = 0.0f, sky = 0.0f, sky_near = 0.0f;
            float high = 0.0f, high_open = 0.0f;
            float occlusion[kAoLobes] = {}, lobe_total[kAoLobes] = {};
            for (int r = 0; r < (ao ? kProbeRays : kSkyRays); ++r)
            {
                // With the occlusion: the whole sphere, its upper rays to the place's bounds for the sky (each by its
                // cosine to +y), its lower ones no further than the occlusion's radius. Without: the sky's rays alone,
                // spread by that cosine, each counting the same.
                const Vec3 d = ao ? SphereDirection(r, turn) : SkyDirection(r, turn);
                const bool upwards = d[1] > 0.0f;
                const std::optional<Occluders::Hit> hit = occluders.Closest(o, d, upwards ? reach : params.ao_radius);
                ++traced;
                const float share = ao ? d[1] : 1.0f;
                if (upwards)
                {
                    up += share;
                    sky += hit ? 0.0f : share;
                    sky_near += hit && hit->t < params.near_sky ? 0.0f : share;
                }
                if (d[1] > kOpenElevation)
                {
                    // Open: the directions the sun can stand in (a strip of low sky seen under the edge of a roof
                    // opens a place little: by the same shares).
                    high += share;
                    high_open += hit ? 0.0f : share;
                }
                if (ao)
                {
                    // Each lobe weighs the directions by their cosine to its axis (the receiver's hemisphere). An
                    // occluder at distance t counts 1 - (t / radius)²: table tops and barrels a little away still
                    // count nearly fully and only the far end of the radius fades. Whichever side of it the ray
                    // meets: the places' models are not wound one way. (A cell behind a wall is dark towards the
                    // room for it; the receivers read one cell off their surface, where no such cell is mixed in.)
                    const float t = hit && hit->t < params.ao_radius ? hit->t / params.ao_radius : 1.0f;
                    for (int k = 0; k < kAoLobes; ++k)
                    {
                        const Vec3 &a = AoLobeAxes()[k];
                        const float w = std::max(d[0] * a[0] + d[1] * a[1] + d[2] * a[2], 0.0f);
                        lobe_total[k] += w;
                        if (t < 1.0f)
                            occlusion[k] += (1.0f - t * t) * w;
                    }
                }
            }
            v.sky_open[c * 2] = Byte(sky / up);
            v.sky_open[c * 2 + 1] = Byte(high > 0.0f ? high_open / high : 0.0f);
            closed[c] = sky <= 0.0f ? 1 : 0;
            if (near)
                probes.near_sky[c] = Byte(sky_near / up);
            if (ao)
                for (int k = 0; k < kAoLobes; ++k)
                    probes.ao.visibility[c * kAoLobes + k] = Byte(1.0f - occlusion[k] / lobe_total[k]);
        }
        rays += traced;
        cpu_us += static_cast<uint64_t>(std::max(0.0, ThreadCpuMs() - began) * 1000.0);
    });
    probes.rays = rays.load();
    probes.cpu_ms = static_cast<double>(cpu_us.load()) / 1000.0;
    CloseInside(v, closed);
    if (ao && !params.walk.empty())
        probes.walk_lift = LiftToWalk(probes.ao, params.walk);
    return probes;
}

uint64_t PlaceKey(const PlaceGeometry &geometry, const ProbeParams &params)
{
    Hasher h;
    h.Value(kPlaceBakeVersion).Value(kProbeRays).Value(kSkyRays);
    h.Span(geometry.positions).Span(geometry.indices);
    // The alpha masks: each one's texels once, then per triangle which mask and its reference; the texture
    // coordinates only where they place a mask.
    std::map<const AlphaMask *, uint32_t> masks;
    for (size_t t = 0; t < geometry.cut.size(); ++t)
    {
        const Occluders::Cut &cut = geometry.cut[t];
        if (!cut.mask)
            continue;
        auto [at, added] = masks.try_emplace(cut.mask.get(), static_cast<uint32_t>(masks.size()));
        if (added)
            h.Value(cut.mask->width).Value(cut.mask->height).Span(std::span<const uint8_t>(cut.mask->alpha));
        h.Value(static_cast<uint64_t>(t)).Value(at->second).Value(cut.reference);
        if (geometry.uvs.size() == geometry.positions.size())
            for (int k = 0; k < 3; ++k)
                h.Span(std::span<const float>(geometry.uvs[geometry.indices[3 * t + k]]));
    }
    h.Span(std::span<const float>(params.lo)).Span(std::span<const float>(params.hi));
    h.Value(params.cell).Value(params.max_cells).Value(params.reach).Value(params.turned).Value(params.near_sky);
    h.Value(params.ao_radius).Span(std::span<const Vec3>(params.walk));
    return h.Result();
}

std::span<const uint8_t> PlaceFile::Find(PlaceSection kind, uint64_t key) const
{
    for (const Section &s : sections_)
        if (s.kind == kind && s.key == key)
            return s.bytes;
    return {};
}

void PlaceFile::Put(PlaceSection kind, uint64_t key, std::vector<uint8_t> bytes, size_t keep)
{
    std::erase_if(sections_, [&](const Section &s) { return s.kind == kind && s.key == key; });
    sections_.push_back({kind, key, std::move(bytes)});
    size_t of_kind = 0;
    for (const Section &s : sections_)
        of_kind += s.kind == kind ? 1 : 0;
    for (auto it = sections_.begin(); of_kind > std::max<size_t>(1, keep) && it != sections_.end();)
        if (it->kind == kind)
        {
            it = sections_.erase(it); // the oldest first
            --of_kind;
        }
        else
            ++it;
}

std::vector<uint8_t> PlaceFile::Encode(uint64_t place_key) const
{
    std::vector<uint8_t> out;
    fx::Put(out, kFileMagic);
    fx::Put(out, kPlaceBakeVersion);
    fx::Put(out, place_key);
    fx::Put(out, static_cast<uint32_t>(sections_.size()));
    for (const Section &s : sections_)
    {
        fx::Put(out, static_cast<uint32_t>(s.kind));
        fx::Put(out, s.key);
        fx::Put(out, static_cast<uint64_t>(s.bytes.size()));
        fx::Put(out, s.bytes.data(), s.bytes.size());
    }
    return out;
}

std::optional<PlaceFile> PlaceFile::Decode(std::span<const uint8_t> bytes, uint64_t place_key, std::string &error)
{
    Reader in{bytes};
    uint32_t magic = 0, version = 0, count = 0;
    uint64_t key = 0;
    if (!in.Take(magic) || !in.Take(version) || !in.Take(key) || !in.Take(count))
    {
        error = "shorter than its header";
        return std::nullopt;
    }
    if (magic != kFileMagic || version != kPlaceBakeVersion || key != place_key)
    {
        error = "another place or version";
        return std::nullopt;
    }
    PlaceFile file;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t kind = 0;
        uint64_t section_key = 0, size = 0;
        if (!in.Take(kind) || !in.Take(section_key) || !in.Take(size) || size > bytes.size() - in.at)
        {
            error = "a section runs past its end";
            return std::nullopt;
        }
        std::vector<uint8_t> data(bytes.begin() + static_cast<std::ptrdiff_t>(in.at),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(in.at + size));
        in.at += size;
        file.sections_.push_back({static_cast<PlaceSection>(kind), section_key, std::move(data)});
    }
    if (in.at != bytes.size())
    {
        error = "bytes after its last section";
        return std::nullopt;
    }
    return file;
}

std::vector<uint8_t> EncodeProbes(const PlaceProbes &probes)
{
    const VisibilityVolume &v = probes.visibility;
    std::vector<uint8_t> out;
    out.reserve(64 + v.sky_open.size() + probes.near_sky.size() + probes.ao.visibility.size());
    fx::Put(out, kProbesMagic);
    fx::Put(out, v.size.data(), 12);
    fx::Put(out, v.origin.data(), 12);
    fx::Put(out, v.cell);
    fx::Put(out, probes.rays);
    const uint8_t parts[2] = {static_cast<uint8_t>(probes.near_sky.empty() ? 0 : 1), static_cast<uint8_t>(probes.ao.Empty() ? 0 : 1)};
    fx::Put(out, parts, 2);
    fx::Put(out, v.sky_open.data(), v.sky_open.size());
    fx::Put(out, probes.near_sky.data(), probes.near_sky.size());
    fx::Put(out, probes.ao.visibility.data(), probes.ao.visibility.size());
    return out;
}

std::optional<PlaceProbes> DecodeProbes(std::span<const uint8_t> bytes, std::string &error)
{
    Reader in{bytes};
    PlaceProbes probes;
    VisibilityVolume &v = probes.visibility;
    uint32_t magic = 0;
    uint8_t parts[2] = {};
    if (!in.Take(magic) || !in.Take(v.size.data(), 12) || !in.Take(v.origin.data(), 12) || !in.Take(v.cell) ||
        !in.Take(probes.rays) || !in.Take(parts, 2) || magic != kProbesMagic)
    {
        error = "no probes header";
        return std::nullopt;
    }
    for (int k = 0; k < 3; ++k)
        if (v.size[k] <= 0 || v.size[k] > kProbeMaxSide)
        {
            error = "a grid no 3D texture holds";
            return std::nullopt;
        }
    const size_t cells = v.Cells();
    if (!(v.cell > 0.0f) || parts[0] > 1 || parts[1] > 1 ||
        bytes.size() - in.at != cells * (2 + (parts[0] ? 1 : 0) + (parts[1] ? kAoLobes : 0)))
    {
        error = "its size does not match its cells";
        return std::nullopt;
    }
    v.sky_open.resize(cells * 2);
    in.Take(v.sky_open.data(), v.sky_open.size());
    if (parts[0])
    {
        probes.near_sky.resize(cells);
        in.Take(probes.near_sky.data(), cells);
    }
    if (parts[1])
    {
        probes.ao.size = v.size;
        probes.ao.origin = v.origin;
        probes.ao.cell = v.cell;
        probes.ao.visibility.resize(cells * kAoLobes);
        in.Take(probes.ao.visibility.data(), probes.ao.visibility.size());
    }
    return probes;
}

} // namespace storm::fx
