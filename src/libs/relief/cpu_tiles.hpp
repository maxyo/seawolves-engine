#pragma once

#include <algorithm>
#include <string_view>

namespace storm::relief
{
// Physical RAM is not the free memory. Even a high-RAM phone must bound a single convolution's activation size. The
// model, padding and output resolution stay unchanged.
inline int DefaultCpuTile(double ram_gb) { return ram_gb <= 4.5 ? 128 : 256; }

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
    if (effective <= 32) return 0;
    const int candidate = std::min(256, std::max(32, effective / 2 / 32 * 32));
    return candidate < effective ? candidate : 0;
}
} // namespace storm::relief
