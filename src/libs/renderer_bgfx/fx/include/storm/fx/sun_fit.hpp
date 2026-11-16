// The sun direction a location's lightmaps were baked with.
//
// The towns' ground lightmaps (lighting\<time of day>\...\shadow.tga) carry the shadows of their buildings, walls
// and palms for a sun that is not the game's run-time one (FortFrance's 8:00 set was baked with a sun 69° high from
// the south: the live 8:00 sun stands 28° high in the east). Run-time shadows of the same geometry must fall where the
// ground shows them, so they are cast along the bake's direction.
//
// FitBakedSun ray-casts the location's still geometry (Occluders) from lightmapped ground samples towards candidate
// directions and keeps the one whose "shadowed" agrees best with the lightmap's darkness (point-biserial correlation
// r of shadowed vs luminance): a coarse search over the sky, then a fine one around the best. FortFrance_town: r 0.62
// at the fit, 0.23 along the live sun; SentJons_town at noon 0.65 (the live noon sun 5° off).
#pragma once

#include "storm/fx/bakes.hpp"
#include "storm/fx/cutout.hpp"
#include "storm/fx/sky_model.hpp"

#include <array>
#include <memory>
#include <optional>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace storm::fx
{

// The farthest any ray over an Occluders runs towards the sun (the town light's sun and walls).
inline constexpr float kSunRayReach = 300.0f;

// A place's still geometry for rays: what it draws, with the holes its textures' alpha cuts (one policy for every
// bake: the sky, the sun, the occlusion). One per place, shared by all of them; the rays are answered by
// a bounding volume hierarchy (madmann91/bvh, MIT: THIRD_PARTY.md), any thread at once.
class Occluders
{
  public:
    // Triangles whose texture's alpha cuts holes (foliage, nets): a ray passes where the mask at the hit is at or
    // below the material's reference, as the colour pass's alpha test lets the background through (fx/cutout.hpp).
    struct Cut
    {
        std::shared_ptr<const AlphaMask> mask;
        float reference = 0.5f;
    };
    Occluders(std::span<const Vec3> positions, std::span<const uint32_t> indices);
    // `uvs` per position; `cut` per triangle (an empty mask: solid).
    Occluders(std::span<const Vec3> positions, std::span<const uint32_t> indices, std::span<const std::array<float, 2>> uvs,
              std::span<const Cut> cut);
    ~Occluders();
    Occluders(const Occluders &) = delete;
    Occluders &operator=(const Occluders &) = delete;
    bool Empty() const { return triangles_ == 0; }
    size_t Triangles() const { return triangles_; }
    // What it holds, in bytes (the log).
    size_t Bytes() const;
    // True when the ray from `origin` along unit `dir` hits a triangle within `reach`, but for the triangle numbers in
    // `ignore` (ascending: a lamp's own fixture, lamp_shadows.hpp FindFixtures).
    bool Occluded(const Vec3 &origin, const Vec3 &dir, float reach, std::span<const uint32_t> ignore = {}) const;
    // The nearest triangle the ray hits within `reach` (alpha holes let it through): at origin + t dir, at
    // barycentric (u, v) of the triangle's second and third corners. From either side: the places' models are not
    // wound one way (measured: 37 to 49 % of what a room's rays meet first is met from behind).
    struct Hit
    {
        float t = 0.0f;
        uint32_t triangle = 0;
        float u = 0.0f, v = 0.0f;
    };
    std::optional<Hit> Closest(const Vec3 &origin, const Vec3 &dir, float reach) const;

  private:
    struct Tree;
    std::unique_ptr<Tree> tree_;
    size_t triangles_ = 0;
};

struct SunFitSample
{
    Vec3 position{};     // on the ground
    float luminance = 0; // the lightmap there, 0..1
};

struct SunFit
{
    Vec3 dir{0.0f, 1.0f, 0.0f}; // unit vector towards the sun
    float r = 0.0f;             // agreement of the shadow mask with the lightmap (point-biserial correlation)
    float shadowed = 0.0f;      // share of the samples in its shadow
    bool valid = false;         // r below kSunFitMinR or no samples: no direction
};

inline constexpr float kSunFitMinR = 0.3f;

// `samples`: about a thousand spread over the lightmapped ground (more add little); `reach` in metres; the candidate
// directions are shared out over `threads`. A town (1500 samples): about 0.45 million rays.
SunFit FitBakedSun(const Occluders &occluders, std::span<const SunFitSample> samples, float reach = 200.0f,
                   unsigned threads = 1);

} // namespace storm::fx
