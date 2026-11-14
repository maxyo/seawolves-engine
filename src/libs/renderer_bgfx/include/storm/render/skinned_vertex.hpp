#pragma once
// The layout of a GPU-skinned source vertex (GEOS::AVERTEX0, the model's gpu_skinning.hpp copies it into a static
// vertex buffer), shared by the model and the renderer's effects (the characters' skin colour, effects/lighting.cpp).
#include <cstdint>

namespace storm::render
{

inline constexpr uint32_t kSkinnedSourceStride = 44;
inline constexpr uint32_t kSkinnedWeightOffset = 12; // float: the first bone's blend weight
inline constexpr uint32_t kSkinnedBonesOffset = 16;  // uint8 x 4: bone 0, bone 1, then
inline constexpr uint32_t kSkinWeightByte = 2;       // the skin weight (a_indices.z in the shader) and
inline constexpr uint32_t kTorsoWeightByte = 3;      // the torso weight (a_indices.w), 0..255, written by the model
inline constexpr uint32_t kSkinnedNormalOffset = 20; // float x 3
inline constexpr uint32_t kSkinnedUvOffset = 36;     // float x 2

} // namespace storm::render
