#include "storm/fx/visibility.hpp"

#include <algorithm>
#include <cmath>

namespace storm::fx
{

namespace
{

// The eight cells around `world` and their trilinear weights.
template <class Read> void Trilinear(const VisibilityVolume &v, const Vec3 &world, Read &&read)
{
    float f[3];
    int i0[3], i1[3];
    for (int k = 0; k < 3; ++k)
    {
        const float c = std::clamp((world[k] - v.origin[k]) / v.cell - 0.5f, 0.0f, static_cast<float>(v.size[k] - 1));
        i0[k] = static_cast<int>(std::floor(c));
        i1[k] = std::min(i0[k] + 1, v.size[k] - 1);
        f[k] = c - static_cast<float>(i0[k]);
    }
    for (int corner = 0; corner < 8; ++corner)
    {
        const int x = corner & 1 ? i1[0] : i0[0], y = corner & 2 ? i1[1] : i0[1], z = corner & 4 ? i1[2] : i0[2];
        const float w = (corner & 1 ? f[0] : 1.0f - f[0]) * (corner & 2 ? f[1] : 1.0f - f[1]) * (corner & 4 ? f[2] : 1.0f - f[2]);
        read((static_cast<size_t>(z) * v.size[1] + y) * v.size[0] + x, w);
    }
}

} // namespace

std::array<float, 2> VisibilityVolume::Sample(const Vec3 &world) const
{
    if (Empty())
        return {1.0f, 1.0f};
    std::array<float, 2> out{0.0f, 0.0f};
    Trilinear(*this, world, [&](size_t cell, float w) {
        out[0] += w * sky_open[cell * 2] / 255.0f;
        out[1] += w * sky_open[cell * 2 + 1] / 255.0f;
    });
    return out;
}

float SampleCells(const VisibilityVolume &volume, const std::vector<uint8_t> &values, const Vec3 &world)
{
    if (volume.Empty() || values.size() != volume.Cells())
        return 1.0f;
    float out = 0.0f;
    Trilinear(volume, world, [&](size_t cell, float w) { out += w * values[cell] / 255.0f; });
    return out;
}

} // namespace storm::fx
