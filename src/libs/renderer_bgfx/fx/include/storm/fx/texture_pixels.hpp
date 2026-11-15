// A texture's colours at a small level, on the CPU (src/effects/texture_pixels.hpp makes them from the game's file):
// 3 bytes per texel, as stored.
#pragma once

#include <cstdint>
#include <vector>

namespace storm::fx
{

struct TexturePixels
{
    int width = 0, height = 0;
    std::vector<uint8_t> srgb;
    bool Empty() const { return srgb.empty(); }
};

} // namespace storm::fx
