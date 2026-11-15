#pragma once

#include <algorithm>
#include <string_view>

namespace storm::relief
{
// Physical RAM is not the free space in ORT's shared arena. Even a high-RAM phone must bound a single
// convolution's activation size. The model, padding and output resolution stay unchanged.
inline int DefaultCpuTile(double ram_gb) { return ram_gb <= 4.5 ? 128 : 256; }

inline bool CpuAllocationFailure(std::string_view error)
{
    // ORT often wraps this in a QLinearConv ORT_FAIL, so the status code alone does not identify OOM.
    // Unknown execution/shape/model errors must not be hidden by a retry.
    return error.find("onnxruntime:") != error.npos &&
           ((error.find("BFCArena::AllocateRawInternal") != error.npos &&
             error.find("Available memory of ") != error.npos &&
             error.find("is smaller than requested bytes of ") != error.npos) ||
            error.find("Failed to allocate memory for requested buffer of size") != error.npos);
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
