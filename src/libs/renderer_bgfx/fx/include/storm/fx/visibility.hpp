// How open to the sky every point of a location is: one volume for towns, interiors, ship decks and holds, part of
// the place's probes (fx/place_probes.hpp: baked from what the place draws, its textures' holes cut) and sampled per
// pixel.
//
// Per cell, from its centre:
//   * sky (first byte, R of the RG8 texture): the cosine-weighted share of the upper hemisphere (around +y) that reaches past the place's geometry;
//     the rays run to the place's bounds (at most the probes' reach), so a hall or a cave chamber with a ceiling 12 m
//     up reads closed;
//   * open (second byte, G): the share of the directions more than 15° above the horizon that reach out, weighed as the sky:
//     whether a sun in the sky can get here at all (an awning, an arcade, a gallery close it; a narrow alley keeps some).
// A cell whose centre lies inside the geometry (every ray up closed) takes the most closed of its open neighbours: a
// wall's cells must not carry the open outside into the room (receivers also read one cell off their surface,
// ff_fragment.sh ffVisibility).
#pragma once

#include "storm/fx/sky_model.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace storm::fx
{

struct VisibilityVolume
{
    std::array<int, 3> size{0, 0, 0}; // cells along x, y, z
    Vec3 origin{0.0f, 0.0f, 0.0f};    // world corner of cell (0, 0, 0)
    float cell = 0.0f;
    std::vector<uint8_t> sky_open;    // 2 per cell, x fastest, then y, then z

    bool Empty() const { return sky_open.empty(); }
    size_t Cells() const { return static_cast<size_t>(size[0]) * size[1] * size[2]; }
    // Trilinear between cell centres, clamped to the grid, as the GPU samples it: {sky, open}; {1, 1} when empty.
    std::array<float, 2> Sample(const Vec3 &world) const;
};

// `values` (one byte per cell of `volume`) read like Sample, 0..1.
float SampleCells(const VisibilityVolume &volume, const std::vector<uint8_t> &values, const Vec3 &world);

} // namespace storm::fx
