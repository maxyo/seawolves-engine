// The tracks behind the hulls for fx::SeaWakes, without the GPU: where each hull's bow has been, and the ribbon
// along that track on which its Kelvin waves are drawn (shaders/fs_fx_wake.sc, include/fx_kelvin.sh).
//
// A track is a polyline of the bow's positions, a point every kSpacing metres, newest first; the bow where it is now
// heads it. The ribbon follows it back from the bow: at each point two vertices, one to each side, as far out as the
// waves reach there (the Kelvin wedge, 19.47 degrees, plus the beam and a margin), never farther than the track's
// turning radius (so the inner side of a turn does not fold over). Each vertex carries its distance behind the bow
// along the track and to the side, the unit vector pointing back along the track there, the wavenumber and height of
// the waves for the speed the hull had when it passed, and how much of them is left with age. The waves therefore
// stay in the water the hull sailed through: a turn bends the wake behind it, it does not swing the whole V round.
#pragma once

#include "storm/fx/wave_source.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace storm::fx
{

struct WakeVertex
{
    float x, y, fade;           // clip position in the wake target; how much of the waves is left (0..1)
    float along, side, k0;      // metres behind the bow along the track, metres to the side; g / U^2 (1/m)
    float back_x, back_z, amplitude; // unit vector back along the track (world x, z); the waves' height (m)
};

class WakePaths
{
  public:
    static constexpr float kSpacing = 3.0f;    // metres of track between points
    static constexpr float kMaxLength = 360.0f; // metres of track a wake keeps
    static constexpr float kLife = 45.0f;      // seconds a point of track is kept
    static constexpr float kFadeAge = 18.0f;   // seconds over which the waves left at a point fade to 1/e
    static constexpr float kMinSpeed = 0.5f;   // m/s: slower hulls lay no track
    static constexpr float kCuspTan = 0.35355339f; // tan(19.47 degrees)
    static constexpr float kGravity = 9.81f;

    struct Point
    {
        double x = 0.0, z = 0.0; // the bow, absolute world metres
        float speed = 0.0f;      // m/s the hull had there
        double time = 0.0;       // game seconds when it passed
    };
    struct Path
    {
        uint64_t identity = 0;
        Point head;                // the bow now
        std::deque<Point> points;  // laid track, newest first
        float length = 1.0f;       // the hull's waterline (m)
        float half_beam = 0.5f;    // m
        double seen = 0.0;         // game seconds of the last update
    };

    // The waves' height for a hull of waterline `length` (m) at `speed` (m/s): grows with U^2 / g, the scale of
    // a ship's waves, and with its Froude number up to the hump at about 0.4.
    static float Amplitude(float speed, float length)
    {
        const float u = std::max(std::fabs(speed), 0.0f);
        const float froude = u / std::sqrt(kGravity * std::max(length, 1.0f));
        return 0.08f * u * u / kGravity * std::clamp(froude * 2.5f, 0.3f, 1.0f);
    }
    static float Wavenumber(float speed)
    {
        const float u = std::max(std::fabs(speed), kMinSpeed);
        return kGravity / (u * u);
    }

    // This frame's hulls (WaveSource::Kind::Hull with an identity; others are ignored) at game time `now`.
    void Update(std::span<const WaveSource> sources, double now)
    {
        for (const WaveSource &s : sources)
        {
            if (s.kind != WaveSource::Kind::Hull || s.identity == 0)
                continue;
            Path *path = Find(s.identity);
            const Point bow{s.position[0] + s.forward[0] * s.stem, s.position[1] + s.forward[1] * s.stem,
                            std::fabs(s.speed), now};
            if (!path)
            {
                paths_.push_back({});
                path = &paths_.back();
                path->identity = s.identity;
                path->points.push_front(bow);
            }
            else if (Distance(path->head, bow) > 50.0)
            {
                // A jump (teleport, a new scene): the old track is somewhere else.
                path->points.clear();
                path->points.push_front(bow);
            }
            path->head = bow;
            path->length = std::max(s.stem - s.stern, 1.0f);
            path->half_beam = std::max(s.half_width, 0.5f);
            path->seen = now;
            // A point every kSpacing metres of the bow's way, only while it makes way.
            if (bow.speed >= kMinSpeed && Distance(path->points.front(), bow) >= kSpacing)
                path->points.push_front(bow);
            else if (bow.speed < kMinSpeed && !path->points.empty() && Distance(path->points.front(), bow) >= kSpacing)
                path->points.front() = bow; // drifting: no waves laid, the track follows
        }
        // Old track, and hulls no longer seen once their waves have gone.
        for (Path &path : paths_)
        {
            float length = 0.0f;
            const Point *previous = &path.head;
            size_t keep = 0;
            for (const Point &p : path.points)
            {
                length += static_cast<float>(Distance(*previous, p));
                previous = &p;
                ++keep;
                if (length > kMaxLength || now - p.time > kLife)
                    break;
            }
            path.points.resize(keep);
        }
        std::erase_if(paths_, [&](const Path &p) { return now - p.seen > kLife; });
    }

    void Reset() { paths_.clear(); }
    const std::vector<Path> &Paths() const { return paths_; }

    // The ribbon of `path` as a triangle strip (two vertices per point, bow first) in a target of `extent` metres
    // centred on `centre` (absolute world x, z); `flip_y`: the target's rows run from the top (not GL). Returns the
    // vertices appended to `out` (0: nothing to draw).
    static size_t Ribbon(const Path &path, double now, std::array<double, 2> centre, float extent, bool flip_y,
                         std::vector<WakeVertex> &out)
    {
        const size_t count = path.points.size() + 1;
        if (count < 2)
            return 0;
        const auto at = [&](size_t i) -> const Point & { return i == 0 ? path.head : path.points[i - 1]; };
        const size_t first = out.size();
        float along = 0.0f;
        float limit = 1e9f; // the narrowest turning radius met so far: a wider ribbon would fold
        for (size_t i = 0; i < count; ++i)
        {
            const Point &p = at(i);
            if (i > 0)
                along += static_cast<float>(Distance(at(i - 1), p));
            // Back along the track: from the newer neighbour to the older one.
            const Point &newer = at(i > 0 ? i - 1 : 0);
            const Point &older = at(i + 1 < count ? i + 1 : count - 1);
            double bx = older.x - newer.x, bz = older.z - newer.z;
            const double norm = std::hypot(bx, bz);
            if (norm < 1e-6)
            {
                if (out.size() == first)
                    continue; // no direction yet at the bow
                bx = out.back().back_x;
                bz = out.back().back_z;
            }
            else
            {
                bx /= norm;
                bz /= norm;
            }
            if (i > 0 && i + 1 < count)
            {
                // The radius of the turn at this point: the spacing over the change of direction.
                const double ax = p.x - newer.x, az = p.z - newer.z, cx = older.x - p.x, cz = older.z - p.z;
                const double la = std::hypot(ax, az), lc = std::hypot(cx, cz);
                if (la > 1e-6 && lc > 1e-6)
                {
                    const double turn = std::acos(std::clamp((ax * cx + az * cz) / (la * lc), -1.0, 1.0));
                    if (turn > 1e-4)
                        limit = std::min(limit, static_cast<float>(0.5 * (la + lc) / turn) * 0.9f);
                }
            }
            const float k0 = Wavenumber(p.speed);
            float width = path.half_beam + along * kCuspTan * 1.3f + 3.0f / k0;
            width = std::max(std::min(width, limit), path.half_beam);
            const float age = static_cast<float>(now - p.time);
            float fade = std::exp(-age / kFadeAge);
            // The far end and the oldest points fade out; the bow fades in over its first metres.
            fade *= std::clamp((kMaxLength - along) / (0.25f * kMaxLength), 0.0f, 1.0f);
            fade *= std::clamp((kLife - age) / (0.25f * kLife), 0.0f, 1.0f);
            if (i + 1 == count)
                fade = 0.0f;
            const float amplitude = Amplitude(p.speed, path.length);
            // The side (positive towards (-back z, back x), as fs_fx_wake.sc reads it).
            const double sx = -bz, sz = bx;
            for (const float d : {-width, width})
            {
                const double wx = p.x + sx * d - centre[0], wz = p.z + sz * d - centre[1];
                const float cx = static_cast<float>(wx / (0.5 * extent));
                const float cy = static_cast<float>(wz / (0.5 * extent)) * (flip_y ? -1.0f : 1.0f);
                out.push_back({cx, cy, fade, along, d, k0, static_cast<float>(bx), static_cast<float>(bz), amplitude});
            }
        }
        return out.size() - first;
    }

  private:
    static double Distance(const Point &a, const Point &b) { return std::hypot(a.x - b.x, a.z - b.z); }
    Path *Find(uint64_t identity)
    {
        for (Path &p : paths_)
            if (p.identity == identity)
                return &p;
        return nullptr;
    }
    std::vector<Path> paths_;
};

} // namespace storm::fx
