#include "storm/fx/ao_volume.hpp"

#include <algorithm>
#include <cmath>

namespace storm::fx
{
const std::array<Vec3, kAoLobes> &AoLobeAxes()
{
    static const float k = 0.57735027f;
    static const std::array<Vec3, kAoLobes> axes = {{{k, k, k}, {k, -k, -k}, {-k, k, -k}, {-k, -k, k}}};
    return axes;
}

float AoVolume::Combine(const float lobes[kAoLobes], Vec3 normal)
{
    float sum = 0.0f, weight = 0.0f;
    for (int i = 0; i < kAoLobes; ++i)
    {
        const Vec3 &a = AoLobeAxes()[i];
        const float w = std::max(normal[0] * a[0] + normal[1] * a[1] + normal[2] * a[2], 0.0f);
        sum += w * lobes[i];
        weight += w;
    }
    return weight > 0.0f ? sum / weight : 1.0f;
}

float AoVolume::At(Vec3 world, Vec3 normal) const
{
    int c[3];
    for (int i = 0; i < 3; ++i)
    {
        c[i] = static_cast<int>(std::floor((world[i] - origin[i]) / cell));
        if (c[i] < 0 || c[i] >= size[i])
            return 1.0f;
    }
    const size_t index = (static_cast<size_t>(c[2]) * size[1] + c[1]) * size[0] + c[0];
    float lobes[kAoLobes];
    for (int k = 0; k < kAoLobes; ++k)
        lobes[k] = visibility[index * kAoLobes + k] / 255.0f;
    return Combine(lobes, normal);
}

float AoVolume::Sample(Vec3 world, Vec3 normal) const
{
    if (Empty())
        return 1.0f;
    int c0[3];
    float f[3];
    for (int i = 0; i < 3; ++i)
    {
        const float u = std::clamp((world[i] - origin[i]) / cell - 0.5f, 0.0f, static_cast<float>(size[i] - 1));
        c0[i] = std::min(static_cast<int>(u), size[i] - 2 < 0 ? 0 : size[i] - 2);
        f[i] = size[i] > 1 ? u - static_cast<float>(c0[i]) : 0.0f;
    }
    float lobes[kAoLobes] = {};
    for (int corner = 0; corner < 8; ++corner)
    {
        float w = 1.0f;
        int c[3];
        for (int i = 0; i < 3; ++i)
        {
            const int bit = (corner >> i) & 1;
            c[i] = std::min(c0[i] + bit, size[i] - 1);
            w *= bit ? f[i] : 1.0f - f[i];
        }
        const size_t index = (static_cast<size_t>(c[2]) * size[1] + c[1]) * size[0] + c[0];
        for (int k = 0; k < kAoLobes; ++k)
            lobes[k] += w * visibility[index * kAoLobes + k] / 255.0f;
    }
    return Combine(lobes, normal);
}

} // namespace storm::fx
