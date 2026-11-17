// The sea's surface on the GPU (the Sea2 / Sea3 vertex ports, shaders/include/sea_surface.sh): what SEA::WaveXZ
// computes on the CPU for every vertex of the sea's mesh, from the same 64 height frames.
//
// The mesh is a flat grid (SEA rebuilds it only when its blocks change); the vertex shader displaces it: for each of
// the two waves it reads the four texels around the point in the two frames it blends, as SEA::FrameBlend blends them
// and SEA::WaveXZ samples them bilinearly. The field is a 2D atlas of RGBA32F texels read with texelFetch, the format
// and binding of the skinned draws' bone palettes (the vertex texture fetch proven on every device): a texel holds the
// frame's height and the per-texel normal SEA::BuildVolumeTexture made (aNormals), as the CPU reads them.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace sea_gpu
{
inline constexpr int32_t kFrames = 64; // SEA FRAMES
inline constexpr int32_t kSize = 128;  // SEA XWIDTH / YWIDTH
inline constexpr int32_t kTilesPerRow = 8; // frame f at tile (f % 8, f / 8)
inline constexpr int32_t kAtlasSize = kSize * kTilesPerRow; // 1024 x 1024
static_assert(kTilesPerRow * kTilesPerRow == kFrames);

// Vertex constant registers of the surface (c4..c8: free in every sea vertex port).
inline constexpr uint32_t kFirstRegister = 4;
inline constexpr uint32_t kRegisters = 5;
//   c4, c5  wave 1, 2: x frame texels per metre, yz the texel coordinate at the mesh's origin (wrapped), w amplitude
//   c6      x, y the frames of waves 1, 2 (0..64), z the vertex shift along the normal (SEA fPosShift * 3), w the
//           bump texture scale (SEA fBumpScale)
//   c7      xy the mesh's origin minus the sea's vertex offset (x, z: the drawn positions are near the camera),
//           zw the bump texture coordinates at the mesh's origin (not wrapped: the foam pass scales them by Sea.FoamUV)
//   c8      xy the camera minus the mesh's origin (x, z), z the sea's reach squared (beyond it the sea is flat, as
//           SEA::WaveXZ returns 0), w the source of the vertices: 1 the shader displaces the flat grid from the field,
//           0 the CPU wrote them displaced (position, normal, bump coordinates; the CPU path, SEA's fallback)
inline constexpr float kSurfaceGpu = 1.0f;
inline constexpr float kSurfaceCpu = 0.0f;

// The atlas texel of texel (x, y) of frame f.
inline size_t atlas_texel(int32_t frame, int32_t x, int32_t y)
{
    const int32_t ax = (frame % kTilesPerRow) * kSize + x, ay = (frame / kTilesPerRow) * kSize + y;
    return static_cast<size_t>(ay) * kAtlasSize + ax;
}

// The atlas, RGBA32F (4 floats a texel): r the height (the byte / 255), g and b the normal's x and z as SEA::FrameBlend
// reads them from aNormals (shorts / 32767.5), a 0. `heights[f]` and `normals[f]` are frame f's kSize x kSize texels.
inline std::vector<float> pack_field(const uint8_t *const *heights, const uint32_t *const *normals)
{
    std::vector<float> texels(static_cast<size_t>(kAtlasSize) * kAtlasSize * 4);
    for (int32_t f = 0; f < kFrames; f++)
        for (int32_t y = 0; y < kSize; y++)
            for (int32_t x = 0; x < kSize; x++)
            {
                const int32_t i = x + y * kSize;
                float *texel = &texels[atlas_texel(f, x, y) * 4];
                texel[0] = static_cast<float>(heights[f][i]) / 255.0f;
                texel[1] = static_cast<float>(static_cast<short>(normals[f][i])) / 32767.5f;
                texel[2] = static_cast<float>(static_cast<short>(normals[f][i] >> 0x10)) / 32767.5f;
                texel[3] = 0.0f;
            }
    return texels;
}

// A wave's register: the texel coordinate of the mesh's origin wrapped to one period of the field (the CPU adds the
// move to the world position before the scale; the wrap keeps the shader's numbers small, same texels).
inline void wave_register(double originX, double originZ, float moveX, float moveZ, float scale, float amplitude,
                          float out[4])
{
    const auto wrap = [](double v) { return static_cast<float>(v - std::floor(v / kSize) * kSize); };
    out[0] = scale;
    out[1] = wrap((originX + moveX) * scale);
    out[2] = wrap((originZ + moveZ) * scale);
    out[3] = amplitude;
}
} // namespace sea_gpu
