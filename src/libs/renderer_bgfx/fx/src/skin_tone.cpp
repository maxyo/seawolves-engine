#include "storm/fx/skin_tone.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace storm::fx
{
namespace
{
float Luminance(float r, float g, float b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}
float Smooth(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

SkinTone MakeSkinTone(std::span<const uint8_t> rgb)
{
    SkinTone tone;
    std::vector<float> rs, gs, lums;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3)
    {
        const float r = rgb[i] / 255.0f, g = rgb[i + 1] / 255.0f, b = rgb[i + 2] / 255.0f;
        const float sum = r + g + b;
        if (sum < 0.03f)
            continue; // black: no chromaticity (shadows, outlines)
        if (!(r >= g && g >= b))
            continue; // not a skin colour of any people (red over green over blue): eyes, lips, hats, cloth at the seams
        rs.push_back(r / sum);
        gs.push_back(g / sum);
        lums.push_back(Luminance(r, g, b));
    }
    // Hair, brows and beards are warm too and the head bones carry them; they are darker than the skin they grow on
    // (blond hair brighter, but rarely as saturated): the brighter 60 % of the warm texels make the reference.
    if (lums.size() >= static_cast<size_t>(kSkinToneMinSamples))
    {
        std::vector<float> order = lums;
        std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(order.size() * 2 / 5), order.end());
        const float floor_lum = order[order.size() * 2 / 5];
        size_t k = 0;
        for (size_t i = 0; i < lums.size(); ++i)
            if (lums[i] >= floor_lum)
            {
                rs[k] = rs[i];
                gs[k] = gs[i];
                lums[k] = lums[i];
                ++k;
            }
        rs.resize(k);
        gs.resize(k);
        lums.resize(k);
    }
    tone.samples = static_cast<int>(rs.size());
    if (tone.samples < kSkinToneMinSamples)
        return tone;
    // The skin is the most common colour under the head and hand bones (a face is mostly skin; hair, brows, beards,
    // hats and the painted shadows spread around it): the peak of the chromaticity histogram (bins of 0.004, summed
    // over 5 x 5 bins), and the samples within 0.03 of it.
    constexpr float kBin = 0.004f;
    constexpr int kBins = 64; // r 0.25 .. 0.506 and g 0.25 .. 0.506 (the plane of warm colours)
    std::vector<int> histogram(kBins * kBins, 0);
    const auto bin = [](float v) { return std::clamp(static_cast<int>((v - 0.25f) / kBin), 0, kBins - 1); };
    for (size_t i = 0; i < rs.size(); ++i)
        ++histogram[bin(gs[i]) * kBins + bin(rs[i])];
    int peak = 0, peak_count = -1;
    for (int y = 0; y < kBins; ++y)
        for (int x = 0; x < kBins; ++x)
        {
            int count = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (x + dx >= 0 && x + dx < kBins && y + dy >= 0 && y + dy < kBins)
                        count += histogram[(y + dy) * kBins + x + dx];
            if (count > peak_count)
            {
                peak_count = count;
                peak = y * kBins + x;
            }
        }
    const float mr = 0.25f + (static_cast<float>(peak % kBins) + 0.5f) * kBin, mg = 0.25f + (static_cast<float>(peak / kBins) + 0.5f) * kBin;
    std::vector<float> dist(rs.size());
    for (size_t i = 0; i < rs.size(); ++i)
        dist[i] = (rs[i] - mr) * (rs[i] - mr) + (gs[i] - mg) * (gs[i] - mg);
    const float cut = 0.03f * 0.03f;
    double sr = 0, sg = 0, n = 0;
    for (size_t i = 0; i < rs.size(); ++i)
        if (dist[i] <= cut)
        {
            sr += rs[i];
            sg += gs[i];
            n += 1;
        }
    // Very saturated paint can lie outside the histogram's warm-skin range. Clamping its bin does not make it a
    // usable reference: reject an empty or undersampled peak before computing its mean and covariance.
    if (n < kSkinToneMinSamples)
        return tone;
    tone.r = static_cast<float>(sr / n);
    tone.g = static_cast<float>(sg / n);
    double crr = 0, crg = 0, cgg = 0;
    for (size_t i = 0; i < rs.size(); ++i)
        if (dist[i] <= cut)
        {
            const double dr = rs[i] - tone.r, dg = gs[i] - tone.g;
            crr += dr * dr;
            crg += dr * dg;
            cgg += dg * dg;
        }
    const double floor = static_cast<double>(kSkinToneMinSpread) * kSkinToneMinSpread;
    crr = crr / n + floor;
    cgg = cgg / n + floor;
    crg = crg / n;
    const double det = crr * cgg - crg * crg;
    tone.inv_rr = static_cast<float>(cgg / det);
    tone.inv_rg = static_cast<float>(-crg / det);
    tone.inv_gg = static_cast<float>(crr / det);
    std::sort(lums.begin(), lums.end());
    tone.lum_lo = lums[lums.size() / 2]; // the median: the torso's skin is painted about as bright as the face
    tone.lum_hi = lums[lums.size() - 1 - lums.size() / 20];
    // A grey reference is no skin (skeletons, grey paint, a face in a hood's shadow): it would take the grey of steel,
    // linen and bone for skin.
    tone.valid = std::hypot(tone.r - 1.0f / 3.0f, tone.g - 1.0f / 3.0f) >= kSkinToneMinSaturation;
    return tone;
}

float SkinLikeness(const SkinTone &tone, float r, float g, float b)
{
    if (!tone.valid)
        return 0.0f;
    const float sum = std::max(r + g + b, 1e-4f);
    const float dr = r / sum - tone.r, dg = g / sum - tone.g;
    const float d2 = dr * dr * tone.inv_rr + 2.0f * dr * dg * tone.inv_rg + dg * dg * tone.inv_gg;
    const float lum = Luminance(r, g, b);
    // The torso's skin may be lit or shaded a little differently from the face's paint: a margin around its range.
    const float in_range = Smooth(tone.lum_lo * 0.45f, tone.lum_lo * 0.65f, lum) *
                           (1.0f - Smooth(tone.lum_hi * 1.1f, tone.lum_hi * 1.3f + 0.02f, lum));
    // Skin is never grey: a texel much less saturated than the reference (steel, linen, faded cloth) is not it.
    const float sat_tone = std::hypot(tone.r - 1.0f / 3.0f, tone.g - 1.0f / 3.0f);
    const float sat = std::hypot(r / sum - 1.0f / 3.0f, g / sum - 1.0f / 3.0f);
    const float saturated = Smooth(0.55f, 0.8f, sat / std::max(sat_tone, 1e-3f));
    return (1.0f - Smooth(kSkinToneNear, kSkinToneFar, d2)) * in_range * saturated;
}

std::array<float, 8> PackSkinTone(const SkinTone &tone)
{
    if (!tone.valid)
        return {-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    return {tone.r, tone.g, tone.lum_lo, tone.lum_hi, tone.inv_rr, tone.inv_rg, tone.inv_gg, 1.0f};
}

float TorsoSkinMask(const SkinTone &tone, float torso_weight, float r, float g, float b)
{
    return std::clamp(2.0f * std::clamp(torso_weight, 0.0f, 1.0f) * (tone.valid ? SkinLikeness(tone, r, g, b) : 1.0f),
                      0.0f, 1.0f);
}

void SetSkinToneSampling(std::array<float, 8> &packed, uint32_t width, uint32_t height, uint32_t levels)
{
    if (packed[0] <= 0.0f)
        return; // preserve unknown/disabled masks
    const float top = static_cast<float>(std::max(width, height));
    const float lod = std::max(0.0f, std::log2(std::max(1.0f, top) / static_cast<float>(kSkinTestTexels)));
    if (levels && std::ceil(lod) >= static_cast<float>(levels))
    {
        packed[0] = -1.0f;
        packed[7] = 0.0f;
        return;
    }
    packed[7] = lod;
}

} // namespace storm::fx
