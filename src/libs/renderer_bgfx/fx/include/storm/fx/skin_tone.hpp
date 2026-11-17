// A character's own skin colour, for the skin its torso bones carry (a bare chest, a bare back, bare arms).
//
// The characters' relief fades out on skin (ff_fragment.sh ffSkin). The head, neck and hand bones say where skin is
// (animation skin_bones.hpp); the spine, chest and arm bones carry shirts and coats as often as bare skin, so there the
// texel's colour decides: a texel whose colour is the skin colour of the same atlas (read where the head and hand bones
// are, the face and the hands) counts as skin. Dark and light skin alike: the reference is the character's own. An atlas
// without a usable face or hand reference conservatively loses relief on its torso and arms as well.
//
// The colour is compared in chromaticity (r, g of rgb / (r + g + b): the hue and saturation without the brightness, so
// the shading painted into the texture does not count) against the reference's mean and spread (Mahalanobis distance),
// and in brightness against the reference's range: beige linen and tan leather sit apart from skin in chromaticity even
// where their brightness matches; a texel far less saturated than the skin (steel, linen) is not skin either. Only warm
// texels (red over green over blue) make the reference. ff_fragment.sh ffSkinLikeness is the same test (keep both in
// step).
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace storm::fx
{

struct SkinTone
{
    bool valid = false;
    int samples = 0;
    float r = 0.0f, g = 0.0f;                  // mean chromaticity
    float inv_rr = 0.0f, inv_rg = 0.0f, inv_gg = 0.0f; // inverse covariance of the chromaticity (regularised)
    float lum_lo = 0.0f, lum_hi = 0.0f;        // the median and the 95th percentile of the reference's luminance, 0..1
};

// The atlas level the reference is read from (its larger side at most this, texels), and the level the torso test reads
// (ff_fragment.sh, through the shader's lod: one level coarser, so the grain of leather and dark cloth averages into
// its own colour while a bare chest stays skin; two levels coarser left blocks of a coat's back in the test).
inline constexpr uint32_t kSkinToneLevel = 128;
inline constexpr uint32_t kSkinTestTexels = 64;
// The fewest skin texels a reference needs.
inline constexpr int kSkinToneMinSamples = 24;
// Chromaticity spread added to the measured one (a face painted in one flat colour still accepts its own shading's hue
// drift): a standard deviation of 0.012 on each axis.
inline constexpr float kSkinToneMinSpread = 0.012f;
// The least saturation (distance of the chromaticity from grey) of a reference that counts as skin.
inline constexpr float kSkinToneMinSaturation = 0.025f;
// Mahalanobis distance (squared) where the likeness starts to fall and where it is 0.
// When in doubt the relief goes: wide enough that beige and tan clothes of a character whose skin is
// that colour lose theirs too.
inline constexpr float kSkinToneNear = 2.25f, kSkinToneFar = 6.25f; // 1.5 and 2.5 standard deviations

// `rgb`: the skin texels, 3 bytes each.
SkinTone MakeSkinTone(std::span<const uint8_t> rgb);
// 0..1: how much a texel (0..1 channels) is this skin.
float SkinLikeness(const SkinTone &tone, float r, float g, float b);
// The torso's final skin mask (the shader's ffSkin): unknown skin colour fails closed. A separate clothes atlas can
// therefore lose its torso relief; unverified bare skin must never acquire generated bumps. Non-torso texels stay 0.
float TorsoSkinMask(const SkinTone &tone, float torso_weight, float r, float g, float b);
// The two vec4 of u_fxSkinTone: [0] mean r, mean g, lum_lo, lum_hi; [1] inv_rr, inv_rg, inv_gg, 1 (0: no reference).
// An invalid reference packs -1 in [0].x to request the conservative torso mask; all zero means explicitly disabled.
std::array<float, 8> PackSkinTone(const SkinTone &tone);
// Set the skin test LOD for the albedo texture actually bound. levels==0 means a complete mip chain. A partial chain
// without the coarse colour level fails closed on torso skin instead of silently sampling its original fine grain.
void SetSkinToneSampling(std::array<float, 8> &packed, uint32_t width, uint32_t height, uint32_t levels);

} // namespace storm::fx
