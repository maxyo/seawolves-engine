// What a texture that failed draws with: a model's texture whose file the game does not ship or that cannot be read
// (the renderer's TextureMissing, the geometry's base stage), a texture whose GPU copy could not be made (the shim).
// The failure itself is logged where it happens (ERROR, or a WARNING for a known game data defect); this only decides
// its look on the screen. Before, such a surface sampled the unbound-texture fallback and drew as a black hole.
//
//   Neutral: flat mid-grey: the surface keeps its lighting and shape and does not draw the eye (release builds).
//   Loud:    a magenta and black checker no game texture has: it shows at a glance what failed (debuggable Android
//            builds and host builds; engine.ini missing_texture_look overrides the choice).
#pragma once

#include <array>
#include <cstdint>

namespace storm::render
{

enum class MissingTextureLook
{
    Neutral,
    Loud,
};

// Before the device is created (the renderer, from engine.ini and the build); a change applies to textures that
// fail from then on.
void SetMissingTextureLook(MissingTextureLook look);
MissingTextureLook CurrentMissingTextureLook();

constexpr uint32_t kMissingTextureSize = 8;
// The stand-in's texels, row by row, as D3DCOLOR (0xAARRGGBB, D3DFMT_A8R8G8B8). Opaque: an alpha-tested surface
// whose texture failed is drawn whole.
std::array<uint32_t, kMissingTextureSize * kMissingTextureSize> MissingTextureTexels(MissingTextureLook look);

} // namespace storm::render
