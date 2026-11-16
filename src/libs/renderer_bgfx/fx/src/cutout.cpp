#include "storm/fx/cutout.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace storm::fx
{

float AlphaMask::At(float u, float v) const
{
    if (alpha.empty())
        return 1.0f;
    const auto wrap = [](float t, int size) {
        const int i = static_cast<int>(std::floor((t - std::floor(t)) * static_cast<float>(size)));
        return std::clamp(i, 0, size - 1);
    };
    return static_cast<float>(alpha[static_cast<size_t>(wrap(v, height)) * width + wrap(u, width)]) / 255.0f;
}

bool MakeAlphaMask(std::vector<uint8_t> &&alpha, int width, int height, float reference, AlphaMask &mask)
{
    const size_t texels = static_cast<size_t>(width) * height;
    if (texels == 0 || alpha.size() != texels)
        return false;
    size_t holes = 0;
    for (const uint8_t a : alpha)
        holes += static_cast<float>(a) <= reference * 255.0f ? 1 : 0;
    if (holes * 100 < texels)
        return false;
    mask.width = width;
    mask.height = height;
    mask.alpha = std::move(alpha);
    return true;
}

} // namespace storm::fx
