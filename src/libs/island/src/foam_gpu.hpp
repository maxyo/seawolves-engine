// What the coast foam's vertex shader (shaders/ports/vs_port_coastfoam.sc) reads, without engine dependencies so
// tests/renderer_bgfx/coast_foam_vertex_check draws it as CoastFoam does.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace foam_gpu
{

constexpr int32_t kFoamsPerDraw = 64; // c8.. of vs_port_coastfoam.sc (PORT_VSC_COUNT 72)
constexpr int32_t kRegisters = 8 + kFoamsPerDraw;
constexpr int32_t kRows = 8;          // rows of a strand
constexpr int32_t kFrames = 64;       // the sea's height frames (SEA FRAMES)
constexpr int32_t kSize = 128;        // their side (SEA XWIDTH)
constexpr int32_t kSlices = kFrames / 4;

#pragma pack(push, 1)
struct Vertex
{
    float x0, z0, x1, z1; // the work part's ends
    float tu;             // its texture u
    uint8_t row;          // 0..7
    uint8_t edge;         // the strand's alpha at its ends: 0..4 quarters
    uint8_t foamLow, foamHigh;
};
#pragma pack(pop)
static_assert(sizeof(Vertex) == 24);

// The alpha of column x of iLen at the strand's ends (the CPU path's fAlpha1), in quarters.
inline uint8_t edge_quarters(int32_t x, int32_t iLen)
{
    int32_t quarters = 4;
    if (x <= 4)
        quarters = x;
    if (x >= iLen - 5)
        quarters = (iLen - 1) - x;
    return static_cast<uint8_t>(quarters < 0 ? 0 : quarters > 4 ? 4 : quarters);
}

// c4 / c5: SEA::WaveXZ samples frame texel (x + move.x) * scale (wrapping at kSize); the texture's texel i has its
// centre at (i + 0.5) / kSize. The offset is reduced to 0..1 in double: move grows with the time at sea.
inline void wave_register(float scale, float moveX, float moveZ, float amplitude, float out[4])
{
    const auto offset = [&](float move) {
        const double uv = (static_cast<double>(move) * scale + 0.5) / kSize;
        return static_cast<float>(uv - std::floor(uv));
    };
    out[0] = scale / kSize;
    out[1] = offset(moveX);
    out[2] = offset(moveZ);
    out[3] = amplitude;
}

// The height frames (kFrames of kSize * kSize bytes) as an A8R8G8B8 volume of kSlices slices: frame 4 s + c in slice
// s, channel c (r, g, b, a as the shader samples them).
inline std::vector<uint32_t> pack_frames(const uint8_t *const *frames)
{
    std::vector<uint32_t> texels(static_cast<size_t>(kSize) * kSize * kSlices);
    for (int32_t s = 0; s < kSlices; s++)
        for (int32_t i = 0; i < kSize * kSize; i++)
            texels[static_cast<size_t>(s) * kSize * kSize + i] =
                static_cast<uint32_t>(frames[4 * s + 3][i]) << 24 | static_cast<uint32_t>(frames[4 * s][i]) << 16 |
                static_cast<uint32_t>(frames[4 * s + 1][i]) << 8 | frames[4 * s + 2][i];
    return texels;
}

} // namespace foam_gpu
