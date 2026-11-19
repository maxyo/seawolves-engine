#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace storm::relief
{
// The CPU networks' memory bound (backend.hpp kCpuNetworkBytes) decides their tiles: ncnn's blobs and workspace of one
// run grow with its padded texels (measured: 2.3..2.6 KB a texel with fp32 storage on arm64 and x86-64, at tiles of
// 128..256 texels and 2..8 threads), so the largest tile within the bound follows from the bytes a texel the network
// measured. The model, padding and output stay unchanged; only ncnn's rounding differs between tile sizes.
inline constexpr int kMinCpuTile = 32;  // below it the halo is most of the work: the texture is skipped, logged
inline constexpr int kMaxCpuTile = 256; // above it a tile saves little halo work
// The bytes a texel before a graph has run on this process's CPU (then its own measurement): above every one measured.
inline constexpr double kInitialCpuBytesPerTexel = 2.75 * 1024.0;
// Headroom over the measured bytes a texel (ncnn picks its kernels by the blob's size; more threads, more workspace).
inline constexpr double kCpuTileMargin = 1.15;

// The largest inner tile (a multiple of 8) whose padded run fits `limit` bytes at `bytes_per_texel`; 0 when not even
// kMinCpuTile does.
inline int BoundedCpuTile(double bytes_per_texel, size_t limit, int halo)
{
    const double side = std::sqrt(static_cast<double>(limit) / (bytes_per_texel * kCpuTileMargin));
    const int inner = (static_cast<int>(side) - 2 * halo) / 8 * 8;
    return inner < kMinCpuTile ? 0 : inner;
}

// `extent` texels in equal tiles of at most `ceiling` (no thin tile at the end that is mostly halo), a multiple of 8.
inline int BalancedTile(int extent, int ceiling)
{
    if (extent <= ceiling)
        return extent;
    const int count = (extent + ceiling - 1) / ceiling;
    return std::min(ceiling, ((extent + count - 1) / count + 7) / 8 * 8);
}

// The CPU network's error when ncnn could not allocate (its layers return -100, or the input did not fit).
inline constexpr const char *kCpuOutOfMemory = "ncnn: CPU inference out of memory";

inline bool CpuAllocationFailure(std::string_view error)
{
    // Only an allocation failure is retried at a smaller tile; unknown execution/shape/model errors are not hidden.
    return error == kCpuOutOfMemory;
}

inline int SmallerCpuTile(int current, int width, int height, int halo)
{
    const int largest_inner = std::max(width - 2 * halo, height - 2 * halo);
    // A small image may already be smaller than the configured tile. The retry must actually reduce its shape.
    const int effective = current > 0 ? std::min(current, largest_inner) : largest_inner;
    if (effective <= kMinCpuTile) return 0;
    const int candidate = std::min(kMaxCpuTile, std::max(kMinCpuTile, effective / 2 / 32 * 32));
    return candidate < effective ? candidate : 0;
}
} // namespace storm::relief
