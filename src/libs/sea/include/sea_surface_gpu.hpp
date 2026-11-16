// The sea's surface on the GPU (the Sea2 / Sea3 vertex ports, shaders/include/sea_surface.sh): what SEA::WaveXZ
// computes on the CPU for every vertex of the sea's mesh, from the same 64 height frames.
//
// The mesh is a flat grid (SEA::Realize rebuilds it only when its blocks change); the vertex shader displaces it:
// for each of the two waves it samples the four texels around the point in the two frames it blends, as
// SEA::FrameBlend blends them and SEA::WaveXZ samples them bilinearly. The per-texel normals (SEA::BuildVolumeTexture's
// aNormals) are a function of the frame's own heights, so a texel carries its height and the two height differences
// its normal is made from, exactly (pack_field).
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace sea_gpu
{
inline constexpr int32_t kFrames = 64; // SEA FRAMES
inline constexpr int32_t kSize = 128;  // SEA XWIDTH / YWIDTH

// Vertex constant registers of the surface (c4..c8: free in every sea vertex port).
inline constexpr uint32_t kFirstRegister = 4;
inline constexpr uint32_t kRegisters = 5;
//   c4, c5  wave 1, 2: x frame texels per metre, yz the texel coordinate at the mesh's origin (wrapped), w amplitude
//   c6      x, y the frames of waves 1, 2 (0..64), z the vertex shift along the normal (SEA fPosShift * 3), w the
//           bump texture scale (SEA fBumpScale)
//   c7      xy the mesh's origin minus the sea's vertex offset (x, z: the drawn positions are near the camera),
//           zw the bump texture coordinates at the mesh's origin (not wrapped: the foam pass scales them by Sea.FoamUV)
//   c8      xy the camera minus the mesh's origin (x, z), z the sea's reach squared (beyond it the sea is flat, as
//           SEA::WaveXZ returns 0), w 0

// The texel of frame f at (x, y): R the height byte, G and B the low bytes of dx + 255 and dz + 255 (dx = height at
// x - 1 minus height at x + 1, dz the same along y, wrapping), A their ninth bits (bit 0 dx, bit 1 dz). D3D
// A8R8G8B8 layout: the shader's r, g, b, a.
inline uint32_t texel(const uint8_t *frame, int32_t x, int32_t y)
{
    const auto at = [frame](int32_t i, int32_t j) {
        return static_cast<int32_t>(frame[(i & (kSize - 1)) + (j & (kSize - 1)) * kSize]);
    };
    const int32_t dx = at(x - 1, y) - at(x + 1, y) + 255;
    const int32_t dz = at(x, y - 1) - at(x, y + 1) + 255;
    const auto a = static_cast<uint32_t>((dx >> 8) | ((dz >> 8) << 1));
    return a << 24 | static_cast<uint32_t>(at(x, y)) << 16 | static_cast<uint32_t>(dx & 255) << 8 |
           static_cast<uint32_t>(dz & 255);
}

// The volume's texels, kSize x kSize x kFrames, slice f = frame f.
inline std::vector<uint32_t> pack_field(const uint8_t *const *frames)
{
    std::vector<uint32_t> texels(static_cast<size_t>(kSize) * kSize * kFrames);
    for (int32_t f = 0; f < kFrames; f++)
        for (int32_t y = 0; y < kSize; y++)
            for (int32_t x = 0; x < kSize; x++)
                texels[(static_cast<size_t>(f) * kSize + y) * kSize + x] = texel(frames[f], x, y);
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
