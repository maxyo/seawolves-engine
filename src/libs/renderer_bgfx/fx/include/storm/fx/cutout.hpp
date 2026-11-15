// Materials whose texture alpha cuts holes into what they cover (foliage, nets, gratings, rigging, torn sails): every
// pass that draws depth or coverage instead of the material's colour must cut the same holes the colour pass's alpha
// test cuts. The GPU passes test the texture itself; the CPU passes (the rays of the place's bakes: fx::Occluders) use
// these.
#pragma once

#include "storm/fx/sky_model.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace storm::fx
{

// The alpha of a texture at a size that shows its holes (about 128 texels across), addressed with wrapping
// coordinates like the texture.
struct AlphaMask
{
    int width = 0, height = 0;
    std::vector<uint8_t> alpha; // width * height, row by row
    // The alpha at (u, v), nearest texel, 0..1.
    float At(float u, float v) const;
};

// The mask of an RGBA8 image (R first), or nothing when its alpha cuts no holes: fewer than one texel in a hundred
// at or below `reference` (0..1).
bool MakeAlphaMask(std::span<const uint8_t> rgba, int width, int height, float reference, AlphaMask &mask);

} // namespace storm::fx
