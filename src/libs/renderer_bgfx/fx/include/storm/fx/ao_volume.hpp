// The ambient-occlusion volume of an interior: ambient visibility per cell of the place's probes (fx/place_probes.hpp:
// the same rays as the sky's visibility, an occluder within the radius counted by its distance), cached with them and
// sampled once per pixel by the lit and baked fixed-function variants. It darkens what the baked vertex colours of the
// interiors cannot show: under tables and benches, behind barrels, in corners, under galleries and stairs (the
// colours are smooth and the floors are a few large quads).
//
// A cell stores its obscurance-weighted visibility in four lobes (255 = nothing within the radius); a point on an open
// floor sees about half of the sphere. The receivers read it one cell off their surface, so the cells behind a wall
// or inside a solid (dark towards the room) are not mixed in.
#pragma once

#include "storm/fx/sky_model.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace storm::fx
{

// Visibility per cell in four cosine lobes around the tetrahedron axes AoLobeAxes() (RGBA8 on the GPU): a receiver
// weighs them by its normal, so it sees its own hemisphere (beams above a wall barely darken it, a table top above
// the floor does).
inline constexpr int kAoLobes = 4;

const std::array<Vec3, kAoLobes> &AoLobeAxes();
// What an open floor or wall sees one cell off it (its own plane hides part of the lobes that face it): the
// shader's "no occlusion" (1). Measured with the probes' rays on a room's floor and walls. One cell off, not half:
// the eight cells the filter then mixes all lie before the surface, whatever the grid's place against it.
inline constexpr float kAoOpenSurface = 0.89f;
// The bake raises the whole volume so that the floor one walks on reads as an open floor (ProbeParams::walk), by
// no more than this.
inline constexpr float kAoWalkLift = 2.0f;

struct AoVolume
{
    std::array<int, 3> size{0, 0, 0}; // cells along x, y, z
    Vec3 origin{0.0f, 0.0f, 0.0f};    // world position of the corner of cell (0, 0, 0)
    float cell = 0.0f;                // cell edge in metres
    std::vector<uint8_t> visibility;  // kAoLobes per cell; cells x fastest, then y, then z; 255 = open

    bool Empty() const
    {
        return visibility.empty();
    }
    // Visibility seen by a surface with unit `normal` in the cell containing `world` (nearest; tests and tools). 1
    // outside the grid.
    float At(Vec3 world, Vec3 normal) const;
    // Trilinear, as the GPU samples it (cell centres as samples, clamped to the grid).
    float Sample(Vec3 world, Vec3 normal) const;
    // The lobes weighed by a normal (the shader's ffAoVolume does the same).
    static float Combine(const float lobes[4], Vec3 normal);
};

} // namespace storm::fx
